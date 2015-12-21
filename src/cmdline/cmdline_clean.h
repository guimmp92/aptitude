// cmdline_clean.h                   -*-c++-*-
//
// Copyright 2004 Daniel Burrows
// Copyright (C) 2015 Manuel A. Fernandez Montecelo
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; see the file COPYING.  If not, write to
// the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
// Boston, MA 02111-1307, USA.

#ifndef CMDLINE_CLEAN_H
#define CMDLINE_CLEAN_H

/** \file cmdline_clean.h
 */

int cmdline_clean(int argc, char *argv[], bool simulate);
int cmdline_autoclean(int argc, char *argv[], bool simulate);

#endif // CMDLINE_CLEAN_H
