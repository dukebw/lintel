# Copyright 2018 Brendan Duke.
#
# This file is part of Lintel.
#
# Lintel is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# Lintel is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
# A PARTICULAR PURPOSE. See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# Lintel. If not, see <http://www.gnu.org/licenses/>.

"""Wrapper for the Lintel C extension APIs."""
import _lintel


loadvid = _lintel.loadvid
loadvid_frame_nums = _lintel.loadvid_frame_nums
loadvid_frame_index = _lintel.loadvid_frame_index
get_num_gops = _lintel.get_num_gops
