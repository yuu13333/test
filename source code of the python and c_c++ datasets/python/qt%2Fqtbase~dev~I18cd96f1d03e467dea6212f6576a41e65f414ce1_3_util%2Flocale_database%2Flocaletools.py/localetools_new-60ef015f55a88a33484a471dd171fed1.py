#############################################################################
##
## Copyright (C) 2020 The Qt Company Ltd.
## Contact: https://www.qt.io/licensing/
##
## This file is part of the test suite of the Qt Toolkit.
##
## $QT_BEGIN_LICENSE:GPL-EXCEPT$
## Commercial License Usage
## Licensees holding valid commercial Qt licenses may use this file in
## accordance with the commercial license agreement provided with the
## Software or, alternatively, in accordance with the terms contained in
## a written agreement between you and The Qt Company. For licensing terms
## and conditions see https://www.qt.io/terms-conditions. For further
## information use the contact form at https://www.qt.io/contact-us.
##
## GNU General Public License Usage
## Alternatively, this file may be used under the terms of the GNU
## General Public License version 3 as published by the Free Software
## Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
## included in the packaging of this file. Please review the following
## information to ensure the GNU General Public License requirements will
## be met: https://www.gnu.org/licenses/gpl-3.0.html.
##
## $QT_END_LICENSE$
##
#############################################################################
"""Utilities shared among the CLDR extraction tools.

Functions:
  unicode2hex() -- converts unicode text to UCS-2 in hex form.
  wrap_list() -- map list to comma-separated string, 20 entries per line.

Classes:
  Error -- A shared error class.
  Transcriber -- edit a file by writing a temporary file, then renaming.
  SourceFileEditor -- adds standard prelude and tail handling to Transcriber.
"""

from contextlib import ExitStack, contextmanager
from pathlib import Path
from tempfile import NamedTemporaryFile
from typing import TextIO

class Error (Exception):
    def __init__(self, msg, *args):
        super().__init__(msg, *args)
        self.message = msg
    def __str__(self):
        return self.message

def unicode2hex(s):
    lst = []
    for x in s:
        v = ord(x)
        if v > 0xFFFF:
            # make a surrogate pair
            # copied from qchar.h
            high = (v >> 10) + 0xd7c0
            low = (v % 0x400 + 0xdc00)
            lst.append(hex(high))
            lst.append(hex(low))
        else:
            lst.append(hex(v))
    return lst

def wrap_list(lst):
    def split(lst, size):
        while lst:
            head, lst = lst[:size], lst[size:]
            yield head
    return ",\n".join(", ".join(x) for x in split(lst, 20))


@contextmanager
def AtomicRenameTemporaryFile(originalFile: TextIO, *, prefix: str, dir: Path):
    """
    Context manager that returns a temporary file that replaces the original
    file on successful exit.

    Accepts a file object that should be created from a named file and have
    name property. Yields a temporary file to the user code open for writing.

    On success closes both file objects and moves the content of the temporary
    file to the original location. On error, removes the temporary file keeping
    the original.
    """
    tempFile = NamedTemporaryFile('w', prefix=prefix, dir=dir, delete=False)
    try:
        yield tempFile
        tempFile.close()
        originalFile.close()
        # Move the modified file to the original location
        Path(tempFile.name).rename(originalFile.name)
    except Exception:
        # delete the temporary file in case of error
        tempFile.close()
        Path(tempFile.name).unlink()
        raise


class Transcriber:
    """Helper class to facilitate rewriting source files.

    This class takes care of the temporary file manipulation. Derived
    classes need to implement transcribing of the content, with
    whatever modifications they may want.  Members reader and writer
    are exposed; use writer.write() to output to the new file; use
    reader.readline() or iterate reader to read the original.

    This class is intended to be used as context manager only (inside a
    `with` statement).
    """
    def __init__(self, path: Path, temp_dir: Path):
        self.path = path
        self.tempDir = temp_dir

    def onEnter(self) -> None:
        """
        Called before transferring control to user code.

        This function can be overridden in derived classes to perform actions
        before transferring control to the user code.

        The default implementation does nothing.
        """
        pass

    def onExit(self) -> None:
        """
        Called after return from user code.

        This function can be overridden in derived classes to perform actions
        after successful return from user code.

        The default implementation does nothing.
        """
        pass

    def __enter__(self):
        with ExitStack() as resources:
            # Open the old file
            self.reader = resources.enter_context(open(self.path))
            self.writer = resources.enter_context(
                AtomicRenameTemporaryFile(self.reader, prefix=self.path.name, dir=self.tempDir))

            self.onEnter()

            # prevent resources to be closed on normal exit and make them available
            # inside __exit__()
            self._resources = resources.pop_all()
            return self

    def __exit__(self, exc_type, exc_value, traceback):
        if exc_type is None:
            with self._resources:
               self.onExit()
        else:
            self._resources.__exit__(exc_type, exc_value, traceback)

        return False


class SourceFileEditor (Transcriber):
    """Transcriber with transcription of code around a gnerated block.

    We have a common pattern of source files with a generated part
    embedded in a context that's not touched by the regeneration
    scripts. The generated part is, in each case, marked with a common
    pair of start and end markers. We transcribe the old file to a new
    temporary file; on success, we then remove the original and move
    the new version to replace it.

    This class takes care of transcribing the parts before and after
    the generated content; on creation, an instance will copy the
    preamble up to the start marker; its close() will skip over the
    original's generated content and resume transcribing with the end
    marker. Derived classes need only implement the generation of the
    content in between.

    Callers should call close() on success or cleanup() on failure (to
    clear away the temporary file); see Transcriber.
    """
    def onEnter(self) -> None:
        self.__copyPrelude()

    def onExit(self) -> None:
        self.__copyTail()

    # Implementation details:
    GENERATED_BLOCK_START = '// GENERATED PART STARTS HERE'
    GENERATED_BLOCK_END = '// GENERATED PART ENDS HERE'

    def __copyPrelude(self):
        # Copy over the first non-generated section to the new file
        for line in self.reader:
            self.writer.write(line)
            if line.strip() == self.GENERATED_BLOCK_START:
                break

    def __copyTail(self):
        # Skip through the old generated data in the old file
        for line in self.reader:
            if line.strip() == self.GENERATED_BLOCK_END:
                self.writer.write(line)
                break
        # Transcribe the remainder:
        for line in self.reader:
            self.writer.write(line)
