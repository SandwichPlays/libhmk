# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.

# https://www.desmos.com/calculator/nzl6twp6ui

from decimal import Decimal
from math import log2
import argparse

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-a",
        type=Decimal,
        required=True,
        description="Constant obtained from fitting the curve",
    )
    parser.add_argument(
        "-i", type=int, default=1024, description="Number of entries in the LUT"
    )
    parser.add_argument(
        "-m", type=int, default=10000, description="Maximum scale value of the LUT"
    )
    parser = parser.parse_args()

    a: Decimal = parser.a
    i: int = parser.i
    m: int = parser.m

    lut = []
    for x in range(i):
        numer = Decimal(m) * (Decimal(1) + a * Decimal(x)).log10()
        denom = (Decimal(1) + a * Decimal(i - 1)).log10()
        lut.append(round(numer / denom))

    items = ", ".join(str(x) for x in lut)
    print(f"{{{items}}}")
