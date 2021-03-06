# 
# Copyright (c) 2000 Carnegie Mellon University.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer. 
#
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in
#    the documentation and/or other materials provided with the
#    distribution.
#
# 3. The name "Carnegie Mellon University" must not be used to
#    endorse or promote products derived from this software without
#    prior written permission. For permission or any other legal
#    details, please contact  
#      Office of Technology Transfer
#      Carnegie Mellon University
#      5000 Forbes Avenue
#      Pittsburgh, PA  15213-3890
#      (412) 268-4387, fax: (412) 268-7395
#      tech-transfer@andrew.cmu.edu
#
# 4. Redistributions of any form whatsoever must retain the following
#    acknowledgment:
#    "This product includes software developed by Computing Services
#     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
#
# CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
# THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
# FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
# AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
# OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#
# $Id: Makefile.PL,v 1.14 2006/11/30 17:11:24 murch Exp $
#
use ExtUtils::MakeMaker;
use Config;

my $SASL_INC = $ENV{SASL_INC};
my $SASL_LIB = $ENV{SASL_LIB} || "-lsasl2";
my $OPENSSL_INC = $ENV{OPENSSL_INC};
my $OPENSSL_LIB = $ENV{OPENSSL_LIB};
my $BDB_INC = $ENV{BDB_INC};
my $BDB_LIB = $ENV{BDB_LIB};

my $LIB_RT = $ENV{LIB_RT};

$libs = "-lcyrus";

# See lib/ExtUtils/MakeMaker.pm for details of how to influence
# the contents of the Makefile that is written.
WriteMakefile(
    'NAME'	=> 'Cyrus::SIEVE::managesieve',
    'ABSTRACT'  => 'Cyurs Sieve management interface',
    'VERSION_FROM' => 'managesieve.pm', # finds $VERSION
    'MYEXTLIB'  => '../lib/isieve.o ../lib/lex.o ../lib/mystring.o ../lib/request.o ../../../lib/libcyrus.a ../../../lib/libcyrus_min.a',
    'LIBS'	=> ["$BDB_LIB $SASL_LIB $OPENSSL_LIB $LIB_RT -lssl -lcrypto"], 
    'DEFINE'	=> '-DPERL_POLLUTE',     # e.g., '-DHAVE_SOMETHING' 
    'INC'	=> "-I../lib/ -I../../../lib/ $SASL_INC $OPENSSL_INC",
);
