/** \file test_cmdline_download_progress_display.cc */

// Copyright (C) 2010 Daniel Burrows
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


// Local includes:
#include <cmdline/cmdline_download_progress_display.h>
#include <cmdline/mocks/terminal.h>
#include <cmdline/mocks/transient_message.h>

// System includes:
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using aptitude::cmdline::create_download_progress_display;
using aptitude::views::download_progress;
using boost::make_shared;
using boost::shared_ptr;
using testing::Return;
using testing::Test;
using testing::_;

namespace mocks = aptitude::cmdline::mocks;

namespace
{
  // We pretend this character occupies two cells and all the rest
  // occupy only one.
  const wchar_t two_column_char = L'-';

  struct CmdlineDownloadProgressDisplayTest : public Test
  {
    shared_ptr<mocks::transient_message> msg;
    shared_ptr<mocks::terminal> term;
    shared_ptr<mocks::terminal_locale> term_locale;

    shared_ptr<download_progress> progress;

    CmdlineDownloadProgressDisplayTest()
      : msg(make_shared<mocks::transient_message>()),
        term(mocks::terminal::create()),
        term_locale(mocks::terminal_locale::create()),
        progress(create_download_progress_display(msg, term, term_locale))
    {
      // Expect no occurrences of the terminal routines other than
      // get_screen_width() (normally they would be invoked by the
      // message, but that's a mock in this case).

      // TODO: is there a way to ask the mock to always reject
      // unexpected calls?
      EXPECT_CALL(*term, output_is_a_terminal())
        .Times(0);

      EXPECT_CALL(*term, write_text(_))
        .Times(0);

      EXPECT_CALL(*term, move_to_beginning_of_line())
        .Times(0);

      EXPECT_CALL(*term, flush())
        .Times(0);

      EXPECT_CALL(*term, prompt_for_input(_))
        .Times(0);

      EXPECT_CALL(*term, output(_))
        .Times(0);

      // Make the terminal 20 characters wide by default (the progress
      // display always outputs messages that are as wide as the
      // terminal, so too wide a value would be a pain).
      EXPECT_CALL(*term, get_screen_width())
        .WillRepeatedly(Return(80));


      // Set up the locale to claim that the two-column character
      // occupies two columns.
      EXPECT_CALL(*term_locale, wcwidth(two_column_char))
        .WillRepeatedly(Return(2));
    }
  };
}

// Test that there are no unexpected calls just from creating the
// download progress display.
TEST_F(CmdlineDownloadProgressDisplayTest, CreateDoesNothing)
{
}
