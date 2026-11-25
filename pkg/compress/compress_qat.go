//go:build qat

/*
 * JuiceFS, Copyright 2026 Juicedata, Inc.
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
 */

package compress

/*
#cgo LDFLAGS: -lqat -lusdm
#include "qat_lz4.h"
*/
import "C"

import (
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"

	"github.com/juicedata/juicefs/pkg/utils"
)

// LZ4 blocks of at least qatMinSize bytes (up to 4 MiB) go to Intel QAT when
// JFS_QAT_LZ4=1. The output is a standard LZ4 block, so data stays readable
// without QAT; any QAT failure falls back to the software codec.
const qatMinSize = 64 << 10

var (
	qatWanted          = os.Getenv("JFS_QAT_LZ4") == "1"
	qatOnce            sync.Once
	qatComp, qatDecomp atomic.Bool
	logger             = utils.GetLogger("juicefs")
)

// qatInit runs in the background after the first LZ4 call: `juicefs mount -d`
// forks helper processes that never touch data and must not take QAT
// instances, and qatlib start-up takes seconds (DMA mappings), during which
// software LZ4 keeps serving.
func qatInit() {
	level := 1
	if v, err := strconv.Atoi(os.Getenv("JFS_QAT_LZ4_LEVEL")); err == nil {
		level = v
	}
	n := int(C.qlz4_init(C.int(level)))
	if n <= 0 {
		logger.Warnf("QAT LZ4 unavailable (init %d), using software LZ4", n)
		return
	}
	qatComp.Store(true)
	// QAT does not decode every block liblz4 produces (those fall back to
	// software); JFS_QAT_LZ4_DECOMP=0 skips the attempt on volumes written by
	// software clients.
	qatDecomp.Store(os.Getenv("JFS_QAT_LZ4_DECOMP") != "0")
	logger.Infof("QAT LZ4 enabled: %d instances, level %d, decompress %v", n, level, qatDecomp.Load())
	if os.Getenv("JFS_QAT_LZ4_STATS") == "1" {
		go func() {
			for range time.Tick(30 * time.Second) {
				var s C.struct_qlz4_stats
				C.qlz4_get_stats(&s)
				logger.Infof("QAT LZ4 stats: compress ok %d fallback %d, decompress ok %d fallback %d",
					s.comp_ok, s.comp_fallback, s.decomp_ok, s.decomp_fallback)
			}
		}()
	}
}

func qatLZ4Compress(dst, src []byte) (int, bool) {
	if !qatWanted || len(src) < qatMinSize || len(dst) == 0 {
		return 0, false
	}
	qatOnce.Do(func() { go qatInit() })
	if !qatComp.Load() {
		return 0, false
	}
	n := C.qlz4_compress(unsafe.Pointer(&src[0]), C.int(len(src)), unsafe.Pointer(&dst[0]), C.int(len(dst)))
	return int(n), n > 0
}

func qatLZ4Decompress(dst, src []byte) (int, bool) {
	if !qatWanted || len(dst) < qatMinSize || len(src) == 0 {
		return 0, false
	}
	qatOnce.Do(func() { go qatInit() })
	if !qatDecomp.Load() {
		return 0, false
	}
	n := C.qlz4_decompress(unsafe.Pointer(&src[0]), C.int(len(src)), unsafe.Pointer(&dst[0]), C.int(len(dst)))
	return int(n), n >= 0
}
