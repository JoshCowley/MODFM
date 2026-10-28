/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* No libm on the FM-1: anything here that the integer modules call must link to nothing
 * (tools/build.py checks the image for undefined symbols). */
