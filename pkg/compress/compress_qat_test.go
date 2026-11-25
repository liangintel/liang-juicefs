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

import (
	"bytes"
	"math/rand"
	"testing"

	"github.com/hungys/go-lz4"
)

func qatOrSkip(t *testing.T) {
	qatWanted = true
	qatOnce.Do(qatInit)
	if !qatComp.Load() {
		t.Skip("no usable QAT compression instance")
	}
}

// Text-like data with long repeats and a zero run, so every LZ4 sequence type
// and hardware block boundary shows up.
func qatTestData(size int) []byte {
	words := []string{"juicefs ", "chunk ", "slice ", "block ", "object ", "storage ", "metadata ", "\n"}
	r := rand.New(rand.NewSource(int64(size)))
	var b bytes.Buffer
	for b.Len() < size {
		switch r.Intn(10) {
		case 0:
			b.Write(make([]byte, r.Intn(4096)))
		case 1:
			if b.Len() > 0 {
				off := r.Intn(b.Len())
				b.Write(b.Bytes()[off : off+r.Intn(b.Len()-off)%2048])
			}
		default:
			b.WriteString(words[r.Intn(len(words))])
		}
	}
	return b.Bytes()[:size]
}

func TestQATLZ4Interop(t *testing.T) {
	qatOrSkip(t)
	c := NewCompressor("lz4")
	for _, size := range []int{qatMinSize, 1 << 20, 4 << 20} {
		src := qatTestData(size)
		dst := make([]byte, c.CompressBound(size))
		n, ok := qatLZ4Compress(dst, src)
		if !ok {
			t.Fatalf("size %d: QAT compression failed", size)
		}

		// The output must be a standard LZ4 block that software clients can read.
		out := make([]byte, size)
		m, err := lz4.DecompressSafe(dst[:n], out)
		if err != nil || m != size || !bytes.Equal(out, src) {
			t.Fatalf("size %d: liblz4 cannot decode QAT output: n=%d err=%v", size, m, err)
		}

		clear(out)
		m, ok = qatLZ4Decompress(out, dst[:n])
		if !ok || m != size || !bytes.Equal(out, src) {
			t.Fatalf("size %d: QAT cannot decode its own output: n=%d ok=%v", size, m, ok)
		}

		clear(out)
		m, err = c.Decompress(out, dst[:n])
		if err != nil || m != size || !bytes.Equal(out, src) {
			t.Fatalf("size %d: decompressing QAT output: n=%d err=%v", size, m, err)
		}

		// Blocks written by software clients must stay readable.
		n, err = lz4.CompressDefault(src, dst)
		if err != nil {
			t.Fatalf("size %d: liblz4 compress: %s", size, err)
		}
		clear(out)
		m, err = c.Decompress(out, dst[:n])
		if err != nil || m != size || !bytes.Equal(out, src) {
			t.Fatalf("size %d: decompressing liblz4 output: n=%d err=%v", size, m, err)
		}
	}
}

func TestQATLZ4SmallBlocksUseSoftware(t *testing.T) {
	qatOrSkip(t)
	src := qatTestData(qatMinSize - 1)
	if _, ok := qatLZ4Compress(make([]byte, len(src)*2), src); ok {
		t.Fatal("blocks below qatMinSize should not be sent to QAT")
	}
	testCompress(t, NewCompressor("lz4"))
}
