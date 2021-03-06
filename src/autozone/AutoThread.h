/*
 * Copyright (c) 2009 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */
/*
    AutoThread.h
    Copyright (c) 2004-2009 Apple Inc. All rights reserved.
 */

#pragma once
#ifndef __AUTO_THREAD__
#define __AUTO_THREAD__


#include "AutoDefs.h"
#include "AutoPointerHash.h"
#include "AutoLock.h"
#include "AutoFreeList.h"

namespace Auto {

    //
    // Forward declarations
    //
    class MemoryScanner;
    class Zone;

    //
    // LocalBlocksHash
    // class for holding per-thread objects and for each, two marks
    // XXX todo: simplify AutoPointerHash to be/use standard C++ hash table
    //
    class LocalBlocksHash : public AutoPointerHash {
    public:
        enum {
            FlagScanned = 0x1,
            FlagMarked = 0x2,
            LocalAllocationsLimit = 2000, // Upper bound on local block count to keep hash buffer size small. Threshold TLC triggers well below this.
        };
        
        LocalBlocksHash(int initialCapacity) : AutoPointerHash(initialCapacity) {}
        
        inline void setScanned(uint32_t index) { setFlag(index, FlagScanned); }
        inline void setScanned(void *p) { int32_t i = slotIndex(p); if (i != -1) setScanned(i); }
        inline bool wasScanned(uint32_t index) { return flagSet(index, FlagScanned); }
        
        inline void setMarked(uint32_t index) { setFlag(index, FlagMarked); }
        inline void setMarked(void *p) { int32_t i = slotIndex(p); if (i != -1) setMarked(i); }
        inline bool wasMarked(uint32_t index) { return flagSet(index, FlagMarked); }
        
        inline bool testAndSetMarked(uint32_t index) {
            bool old = wasMarked(index);
            if (!old) setMarked(index);
            return old;
        }

        // Shark says all these loads are expensive.
        inline void *markedPointerAtIndex(uint32_t index) {
            vm_address_t value = _pointers[index];
            void *pointer = (void *) (value & ~FlagsMask);
            return ((value & FlagMarked) ? pointer : NULL);
        }
        
        inline void *unmarkedPointerAtIndex(uint32_t index) {
            vm_address_t value = _pointers[index];
            void *pointer = (void *) (value & ~FlagsMask);
            return ((value & FlagMarked) ? NULL : ((value == (vm_address_t)RemovedEntry) ? NULL : pointer));
        }
        
        inline void *markedUnscannedPointerAtIndex(uint32_t index) {
            vm_address_t value = _pointers[index];
            void *pointer = (void *) (value & ~FlagsMask);
            return ((value & (FlagMarked|FlagScanned)) == FlagMarked ? pointer : NULL);
        }
        
        inline void clearFlagsCompact() { compact(FlagScanned | FlagMarked); }
        inline bool isFull() { return count() >= Environment::local_allocations_size_limit; }
    };
    
    //
    // SimplePointerBuffer
    // class used to hold list of garbage.
    // XXX replace with std C++
    // 
    class SimplePointerBuffer {
        enum {
            PointerCount = 32
        };
        
        int16_t _cursor;
        int16_t _count;
        SimplePointerBuffer *_overflow;
        void *_pointers[PointerCount];
        
    public:
        SimplePointerBuffer() : _cursor(0), _count(0), _overflow(NULL) {}
        ~SimplePointerBuffer() { if (_overflow) delete _overflow; }
        
        void reset();
        void push(void *p);
        void *pop();
        int16_t count() const { return _count; }
    };


    //----- NonVolatileRegisters -----//
    
    //
    // Used to capture the register state of the current thread.
    //

#if defined(__ppc__) || defined(__ppc64__)
    
    class NonVolatileRegisters {

      private:
      
        enum {
            first_nonvolatile_register = 13,                // used to capture registers
            number_of_nonvolatile_registers = 32 - first_nonvolatile_register,
                                                            // used to capture registers
        };
        
        usword_t _registers[number_of_nonvolatile_registers];// buffer for capturing registers
        
        //
        // capture_registers
        //
        // Capture the state of the non-volatile registers.
        //
        static inline void capture_registers(register usword_t *registers) {
#if defined(__ppc__)
            __asm__ volatile ("stmw r13,0(%[registers])" : : [registers] "b" (registers) : "memory");
#else
            __asm__ volatile ("std r13,0(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r14,8(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r15,16(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r16,24(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r17,32(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r18,40(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r19,48(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r20,56(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r21,64(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r22,72(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r23,80(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r24,88(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r25,96(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r26,104(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r27,112(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r28,120(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r29,128(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r30,136(%[registers])" : : [registers] "b" (registers) : "memory");
            __asm__ volatile ("std r31,144(%[registers])" : : [registers] "b" (registers) : "memory");
#endif
        }

      public:
      
        //
        // Constructor
        //
        NonVolatileRegisters() { capture_registers(_registers); }
        
        
        //
        // buffer_range
        //
        // Returns the range of captured registers buffer.
        //
        inline Range buffer_range() { return Range(_registers, sizeof(_registers)); }
        
    };

#elif defined(__i386__)
    
    class NonVolatileRegisters {
      private:
        // Non-volatile registers are: ebx, ebp, esp, esi, edi
        usword_t _registers[5];  // buffer for capturing registers
        
        //
        // capture_registers
        //
        // Capture the state of the non-volatile registers.
        //
        static inline void capture_registers(register usword_t *registers) {
            __asm__ volatile ("mov %%ebx,  0(%[registers]) \n" 
                              "mov %%ebp,  4(%[registers]) \n" 
                              "mov %%esp,  8(%[registers]) \n" 
                              "mov %%esi, 12(%[registers]) \n" 
                              "mov %%edi, 16(%[registers]) \n" 
                              : : [registers] "a" (registers) : "memory");
        }

      public:
      
        //
        // Constructor
        //
        NonVolatileRegisters() { capture_registers(_registers); }
        
        
        //
        // buffer_range
        //
        // Returns the range of captured registers buffer.
        //
        inline Range buffer_range() { return Range(_registers, sizeof(_registers)); }
        
    };

#elif defined(__x86_64__)
    
    class NonVolatileRegisters {
      private:
        // Non-volatile registers are: rbx rsp rbp r12-r15
        usword_t _registers[7];  // buffer for capturing registers
        
        //
        // capture_registers
        //
        // Capture the state of the non-volatile registers.
        //
        static inline void capture_registers(register usword_t *registers) {
            __asm__ volatile ("movq %%rbx,  0(%[registers]) \n" 
                              "movq %%rsp,  8(%[registers]) \n" 
                              "movq %%rbp, 16(%[registers]) \n" 
                              "movq %%r12, 24(%[registers]) \n" 
                              "movq %%r13, 32(%[registers]) \n" 
                              "movq %%r14, 40(%[registers]) \n" 
                              "movq %%r15, 48(%[registers]) \n" 
                              : : [registers] "a" (registers) : "memory");
        }

      public:
      
        //
        // Constructor
        //
        NonVolatileRegisters() { capture_registers(_registers); }
        
        
        //
        // buffer_range
        //
        // Returns the range of captured registers buffer.
        //
        inline Range buffer_range() { return Range(_registers, sizeof(_registers)); }
        
    };

#else
#error Unknown Architecture
#endif


    //----- Thread -----//
    
    //
    // Track threads that need will be scanned during gc.
    //
    
    class ThreadMemoryAllocator {
        Zone *_zone;
    public:
        ThreadMemoryAllocator(Zone *zone) : _zone(zone) {}
        void *allocate_memory(usword_t size);
        void deallocate_memory(void *address, usword_t size);
        void uncommit_memory(void *address, usword_t size) { Auto::uncommit_memory(address, size); }
        void copy_memory(void *dest, void *source, usword_t size) { Auto::copy_memory(dest, source, size); }
    };

    class EnliveningQueue : public PointerQueue<ThreadMemoryAllocator> {
        LockedBoolean _needs_enlivening;
    public:
        EnliveningQueue(ThreadMemoryAllocator allocator) : PointerQueue<ThreadMemoryAllocator>(allocator), _needs_enlivening() {}
        LockedBoolean &needs_enlivening() { return _needs_enlivening; }
    };
    
    union ThreadState;
    
    class Thread : public AuxAllocated {
    
      private:
            
        Thread      *_next;                                 // next thread in linked list
        Zone        *_zone;                                 // managing zone
        pthread_t   _pthread;                               // posix thread
        mach_port_t _thread;                                // mach thread
        void        *_stack_base;                           // cached thread stack base (pthread_get_stackaddr_np(_pthread)).
        LockedBoolean _scanning;                            // if state is true, collector is scanning, unbind will block.
        uint32_t    _suspended;                             // records suspend count.
        void        *_stack_scan_peak;                      // lowest scanned stack address, for stack clearing
        FreeList    _allocation_cache[maximum_quanta + 1];  // free lists, one for each quanta size, slot 0 is for large clumps
        
        LocalBlocksHash _localAllocations;                  // scanned buffer holding locally allocated blocks
        uint32_t     _localAllocationThreshold;             // trigger TLC when # of local allocation exceeds this value.

        EnliveningQueue _enlivening_queue;                  // queue of pointers to objects to scan at end.
        int32_t     _destructor_count;                      // tracks the number of times the pthread's key destructor has been called
        
        bool        _in_collector;                          // used to indicate that a thread is running inside the collector itself
        
        void get_register_state(ThreadState &state, unsigned &user_count);

        //
        // remove_local
        //
        // remove block from local set.  Assumes its there.
        //
        inline void remove_local(void *block) {
            _localAllocations.remove(block);
        }


      public:
      
        //
        // Constructor. Makes a Thread which is bound to the calling pthread.
        //
        Thread(Zone *zone);
        ~Thread();
        
        //
        // bind
        //
        // Associate the Thread with the calling pthread.
        // This declares the Zone's interest in scanning the calling pthread's stack during collections.
        //
        void bind();
        
        //
        // unbind
        //
        // Disassociate the Thread from the calling pthread.
        // May only be called from the same pthread that previously called bind().
        // unbind() synchronizes with stack scanning to ensure that if a stack scan is in progress
        // the stack will remain available until scanning is complete. Returns true if the thread
        // object can be immediately deleted.
        //
        bool unbind();
        
        //
        // lockForScanning
        //
        // Locks down a thread before concurrent scanning. This blocks a concurrent call to
        // unbind(), so a pthread cannot exit while its stack is being concurrently scanned.
        // Returns true if the thread is currently bound, and thus is known to have a valid stack.
        //
        bool lockForScanning();
        
        //
        // unlockForScanning
        //
        // Relinquishes the scanning lock, which unblocks a concurrent call to unbind().
        //
        void unlockForScanning();
        
        //
        // Accessors
        //
        inline Thread      *next()                { return _next; }
        inline Zone        *zone()                { return _zone; }
        inline pthread_t   pthread()              { return _pthread; }
        inline mach_port_t thread()               { return _thread; }
        inline void        set_next(Thread *next) { _next = next; }
        inline FreeList    &allocation_cache(usword_t index)    { return _allocation_cache[index]; }
        inline void        *stack_base()          { return _stack_base; }
        inline LocalBlocksHash &locals()          { return _localAllocations; }
        inline uint32_t   localAllocationThreshold() const { return _localAllocationThreshold; }
        inline void       setLocalAllocationThreshold(uint32_t threshold) { _localAllocationThreshold = threshold; }
        inline bool       is_bound()              { return _pthread != NULL; }
        inline int32_t    increment_tsd_count()   { return ++_destructor_count; }
        inline void       set_in_collector(bool value) { _in_collector = value; }
        inline bool       in_collector() const    { return _in_collector; }
        
        //
        // Per-thread envlivening, to reduce lock contention across threads while scanning.
        // These are manipulated by Zone::set_needs_enlivening() / clear_needs_enlivening().
        //
        // FIXME:  can we make this lockless altogether?
        //
        EnliveningQueue   &enlivening_queue()     { return _enlivening_queue; }
        LockedBoolean     &needs_enlivening()     { return _enlivening_queue.needs_enlivening(); }

        //
        // clear_stack
        //
        // clears stack memory from the current sp to the depth that was scanned by the last collection
        //
        void clear_stack();
        
        //
        // is_current_stack_address
        //
        // If the current thread is registered with the collector, returns true if the given address is within the address
        // range of the current thread's stack. This code assumes that calling pthread_getspecific() is faster than calling
        // pthread_get_stackaddr_np() followed by pthread_get_stacksize_np().
        //
        inline bool is_stack_address(void *address) {
            Range stack(__builtin_frame_address(0), _stack_base);
            return (stack.in_range(address));
        }
        
        //
        // block_escaped
        //
        // a block is escaping the stack; remove it from local set (cheaply)
        //
        void block_escaped(Zone *zone, Subzone *subzone, void *block);

        //
        // check_for_escape
        //
        // an assignment is happening.  Check for an escape, e.g. global = local
        //
        void track_local_assignment(Zone *zone, void *dst, void *value);

        //
        // track_local_memcopy
        //
        // make sure that dest is marked if any src's are local,
        // otherwise escape all srcs that are local
        //
        void track_local_memcopy(Zone *zone, const void *src, void *dst, size_t size);
        
        //
        // add_local_allocation
        //
        // add a block to this thread's set of tracked local allocations
        //
        void add_local_allocation(void *block) {
            // Limit the size of local block set. This should only trigger rarely.
            if (_localAllocations.isFull())
                flush_local_blocks();
            _localAllocations.add(block);
        }
        
        //
        // flush_local_blocks
        //
        // empties the local allocations hash, making all blocks global
        //
        void flush_local_blocks();
        
        //
        // scan_current_thread
        //
        // Scan the current thread stack and registers for block references.
        //
        void scan_current_thread(MemoryScanner &scanner);
        void scan_current_thread(void (^scanner) (Thread *thread, Range &range), void *stack_bottom);
        void scan_current_thread(void (*scanner) (Thread *thread, Range&, void*), void *arg, void *stack_bottom) {
            scan_current_thread(^(Thread *thread, Range &range) { scanner(thread, range, arg); }, stack_bottom);
        }
        
        //
        // scan_other_thread
        //
        // Scan a thread other than the current thread stack and registers for block references.
        //
        void scan_other_thread(MemoryScanner &scanner, bool withSuspend);
        void scan_other_thread(void (^scanner) (Thread *thread, Range &range), bool withSuspend);
        void scan_other_thread(void (*scanner) (Thread *thread, Range&, void*), void *arg, bool withSuspend) {
            scan_other_thread(^(Thread *thread, Range &range) { scanner(thread, range, arg); }, withSuspend);
        }


        //
        // scan_thread
        //
        // Scans the thread's registers and stack for references to GC managed blocks.
        //
        void scan_thread(MemoryScanner &scanner, bool withSuspend) {
            if (is_current_thread())
                scan_current_thread(scanner);
            else
                scan_other_thread(scanner, withSuspend);
        }


        //
        // dump local objects
        //
        // use callout to dump local objects
        //
        void dump(auto_zone_stack_dump stack_dump, auto_zone_register_dump register_dump, auto_zone_node_dump dump_local_block);


        //
        // is_current_thread
        //
        // Returns true if the this thread is the current thread.
        //
        inline bool is_current_thread() const {
            return pthread_self() == _pthread;
        }
        
        
        //
        // thread_cache_add
        //
        // return memory to the thread local cache
        //
        void thread_cache_add(void *block);
        
        
        //
        // unlink
        //
        // Unlink the thread from the list of threads.
        //
        inline void unlink(Thread **link) {
            for (Thread *t = *link; t; link = &t->_next, t = *link) {
                // if found
                if (t == this) {
                    // mend the link
                    *link = t->_next;
                    break;
                }
            }
        }


        //
        // scavenge_threads
        //
        // Walks the list of threads, looking for unbound threads.
        // These are no longer in use, and can be safely deleted.
        //
        static void scavenge_threads(Thread **active_link, Thread **inactive_link) {
            while (Thread *thread = *active_link) {
                SpinLock lock(&thread->_scanning.lock);
                if (!thread->is_bound()) {
                    // remove thread from the active list.
                    *active_link = thread->_next;
                    // put thread on the inactive list.
                    thread->_next = *inactive_link;
                    *inactive_link = thread;
                } else {
                    active_link = &thread->_next;
                }
            }
        }


        //
        // suspend
        //
        // Temporarily suspend the thread from further execution. Logs and terminates process on failure.
        //
        void suspend();


        //
        // resume
        //
        // Resume a suspended thread. Logs and terminates process on failure.
        //
        void resume();


        //
        // description
        //
        // fills in buf with a textual description of the Thread, for debugging
        // returns buf
        //
        char *description(char *buf, size_t bufsz);

    };
};

#endif // __AUTO_THREAD__

