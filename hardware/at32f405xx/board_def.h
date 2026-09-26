/*
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

//--------------------------------------------------------------------+
// ADC Configuration
//--------------------------------------------------------------------+

#if !defined(ADC_NUM_SAMPLE_CYCLES)
// Number of sample cycles for each ADC conversion (~30kHz sweep rate with 4 keys and 16x oversample)
#define ADC_NUM_SAMPLE_CYCLES ADC_SAMPLETIME_1_5
#endif

#if !defined(ADC_OVERSAMPLE_RATIO)
// Hardware oversampling ratio: 16x oversampling in silicon
#define ADC_OVERSAMPLE_RATIO ADC_OVERSAMPLE_RATIO_16
#endif

#if !defined(ADC_OVERSAMPLE_SHIFT)
#if ADC_RESOLUTION == 14
// Hardware oversampling bit shift: 2 bits right shift (16x oversample -> 14-bit output)
#define ADC_OVERSAMPLE_SHIFT ADC_OVERSAMPLE_SHIFT_2
#else
// Hardware oversampling bit shift: 4 bits right shift (16x oversample -> 12-bit output)
#define ADC_OVERSAMPLE_SHIFT ADC_OVERSAMPLE_SHIFT_4
#endif
#endif

// ADC resolution in bits, set by `scripts/make.py`
#if ADC_RESOLUTION != 12 && ADC_RESOLUTION != 14
#error "Unsupported ADC resolution"
#endif
