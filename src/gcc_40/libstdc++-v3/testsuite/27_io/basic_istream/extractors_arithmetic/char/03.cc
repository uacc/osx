// 1999-04-12 bkoz

// Copyright (C) 1999, 2000, 2002, 2003 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING.  If not, write to the Free
// Software Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307,
// USA.

// 27.6.1.2.2 arithmetic extractors

#include <istream>
#include <sstream>
#include <locale>
#include <testsuite_hooks.h>

bool test03()
{
  std::stringbuf sbuf;
  std::istream istr(&sbuf);
  std::ostream ostr(&sbuf);

  bool test __attribute__((unused)) = true;
  long l01;
  ostr << "12220101";
  istr >> l01; // _M_in_end set completely incorrectly here.
  VERIFY( l01 == 12220101 );
  VERIFY( istr.rdstate() == std::ios_base::eofbit );
  return test;
}

int main()
{
  test03();
  return 0;
}
