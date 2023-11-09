#include <time.h>

static inline uint64_t get_timestamp_counter()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

#include "platform.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "rans_byte.h"

// This is just the sample program. All the meat is in rans_byte.h.

static void panic(const char *fmt, ...)
{
    va_list arg;

    va_start(arg, fmt);
    fputs("Error: ", stderr);
    vfprintf(stderr, fmt, arg);
    va_end(arg);
    fputs("\n", stderr);

    exit(1);
}

static uint8_t* read_file(char const* filename, size_t* out_size)
{
    FILE* f = fopen(filename, "rb");
    if (!f)
        panic("file not found: %s\n", filename);

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* buf = new uint8_t[size];
    if (fread(buf, size, 1, f) != 1)
        panic("read failed\n");

    fclose(f);
    if (out_size)
        *out_size = size;

    return buf;
}

// ---- Stats

struct SymbolStats
{
    uint32_t freqs[256];
    uint32_t cum_freqs[257];

    void count_freqs(uint8_t const* in, size_t nbytes);
    void calc_cum_freqs();
    void normalize_freqs(uint32_t target_total);
};

void SymbolStats::count_freqs(uint8_t const* in, size_t nbytes)
{
    for (int i=0; i < 256; i++)
        freqs[i] = 0;

    for (size_t i=0; i < nbytes; i++)
        freqs[in[i]]++;
}

void SymbolStats::calc_cum_freqs()
{
    cum_freqs[0] = 0;
    for (int i=0; i < 256; i++)
        cum_freqs[i+1] = cum_freqs[i] + freqs[i];
}

void SymbolStats::normalize_freqs(uint32_t target_total)
{
    assert(target_total >= 256);
    
    calc_cum_freqs();
    uint32_t cur_total = cum_freqs[256];
    
    // resample distribution based on cumulative freqs
    for (int i = 1; i <= 256; i++)
        cum_freqs[i] = ((uint64_t)target_total * cum_freqs[i])/cur_total;

    // if we nuked any non-0 frequency symbol to 0, we need to steal
    // the range to make the frequency nonzero from elsewhere.
    //
    // this is not at all optimal, i'm just doing the first thing that comes to mind.
    for (int i=0; i < 256; i++) {
        if (freqs[i] && cum_freqs[i+1] == cum_freqs[i]) {
            // symbol i was set to zero freq

            // find best symbol to steal frequency from (try to steal from low-freq ones)
            uint32_t best_freq = ~0u;
            int best_steal = -1;
            for (int j=0; j < 256; j++) {
                uint32_t freq = cum_freqs[j+1] - cum_freqs[j];
                if (freq > 1 && freq < best_freq) {
                    best_freq = freq;
                    best_steal = j;
                }
            }
            assert(best_steal != -1);

            // and steal from it!
            if (best_steal < i) {
                for (int j = best_steal + 1; j <= i; j++)
                    cum_freqs[j]--;
            } else {
                assert(best_steal > i);
                for (int j = i + 1; j <= best_steal; j++)
                    cum_freqs[j]++;
            }
        }
    }

    // calculate updated freqs and make sure we didn't screw anything up
    assert(cum_freqs[0] == 0 && cum_freqs[256] == target_total);
    for (int i=0; i < 256; i++) {
        if (freqs[i] == 0)
            assert(cum_freqs[i+1] == cum_freqs[i]);
        else
            assert(cum_freqs[i+1] > cum_freqs[i]);

        // calc updated freq
        freqs[i] = cum_freqs[i+1] - cum_freqs[i];
    }
}

int main(int argc, char **argv)
{
    size_t in_size;
    uint8_t* in_bytes = read_file(argv[1], &in_size);

    static const uint32_t prob_bits = 15;
    static const uint32_t prob_scale = 1 << prob_bits;

    SymbolStats stats;
    stats.count_freqs(in_bytes, in_size);
    stats.normalize_freqs(prob_scale);

    // cumlative->symbol table
    // this is super brute force
    uint8_t cum2sym[prob_scale];
    for (int s=0; s < 256; s++)
        for (uint32_t i=stats.cum_freqs[s]; i < stats.cum_freqs[s+1]; i++)
            cum2sym[i] = s;

    static size_t out_max_size = 100<<20; // 100MB
    uint8_t* out_buf = new uint8_t[out_max_size];
    uint8_t* dec_bytes = new uint8_t[in_size];

    // try rANS encode
    uint8_t *rans_begin;
    RansEncSymbol esyms[256];
    RansDecSymbol dsyms[256];

    for (int i=0; i < 256; i++) {
        RansEncSymbolInit(&esyms[i], stats.cum_freqs[i], stats.freqs[i], prob_bits);
        RansDecSymbolInit(&dsyms[i], stats.cum_freqs[i], stats.freqs[i]);
    }

    // ---- regular rANS encode/decode. Typical usage.

    memset(dec_bytes, 0xcc, in_size);

    printf("rANS encode:\n");
    for (int run=0; run < 5; run++) {
        double start_time = timer();
        uint64_t enc_start_time = get_timestamp_counter();

        RansState rans;
        RansEncInit(&rans);

        uint8_t* ptr = out_buf + out_max_size; // *end* of output buffer
        for (size_t i=in_size; i > 0; i--) { // NB: working in reverse!
            int s = in_bytes[i-1];
            RansEncPutSymbol(&rans, &ptr, &esyms[s]);
        }
        RansEncFlush(&rans, &ptr);
        rans_begin = ptr;

        uint64_t enc_clocks = get_timestamp_counter() - enc_start_time;
        double enc_time = timer() - start_time;
        printf("  %"PRIu64" clocks, %.1f clocks/symbol (%5.1fMiB/s)\n", enc_clocks, 1.0 * enc_clocks / in_size, 1.0 * in_size / (enc_time * 1048576.0));
    }
    printf("rANS: %d bytes\n", (int) (out_buf + out_max_size - rans_begin));

    // try rANS decode
    for (int run=0; run < 5; run++) {
        double start_time = timer();
        uint64_t dec_start_time = get_timestamp_counter();

        RansState rans;
        uint8_t* ptr = rans_begin;
        RansDecInit(&rans, &ptr);

        for (size_t i=0; i < in_size; i++) {
            uint32_t s = cum2sym[RansDecGet(&rans, prob_bits)];
            dec_bytes[i] = (uint8_t) s;
            RansDecAdvanceSymbol(&rans, &ptr, &dsyms[s], prob_bits);
        }

        uint64_t dec_clocks = get_timestamp_counter() - dec_start_time;
        double dec_time = timer() - start_time;
        printf("  %"PRIu64" clocks, %.1f clocks/symbol (%5.1fMiB/s)\n", dec_clocks, 1.0 * dec_clocks / in_size, 1.0 * in_size / (dec_time * 1048576.0));
    }

    // check decode results
    if (memcmp(in_bytes, dec_bytes, in_size) == 0)
        printf("decode ok!\n");
    else
        printf("ERROR: bad decoder!\n");

    // ---- interleaved rANS encode/decode. This is the kind of thing you might do to optimize critical paths.

    memset(dec_bytes, 0xcc, in_size);

    // try interleaved rANS encode
    printf("\ninterleaved rANS encode:\n");
    for (int run=0; run < 5; run++) {
        double start_time = timer();
        uint64_t enc_start_time = get_timestamp_counter();

        RansState rans0, rans1;
        RansEncInit(&rans0);
        RansEncInit(&rans1);

        uint8_t* ptr = out_buf + out_max_size; // *end* of output buffer

        // odd number of bytes?
        if (in_size & 1) {
            int s = in_bytes[in_size - 1];
            RansEncPutSymbol(&rans0, &ptr, &esyms[s]);
        }

        for (size_t i=(in_size & ~1); i > 0; i -= 2) { // NB: working in reverse!
            int s1 = in_bytes[i-1];
            int s0 = in_bytes[i-2];
            RansEncPutSymbol(&rans1, &ptr, &esyms[s1]);
            RansEncPutSymbol(&rans0, &ptr, &esyms[s0]);
        }
        RansEncFlush(&rans1, &ptr);
        RansEncFlush(&rans0, &ptr);
        rans_begin = ptr;

        uint64_t enc_clocks = get_timestamp_counter() - enc_start_time;
        double enc_time = timer() - start_time;
        printf("  %"PRIu64" clocks, %.1f clocks/symbol (%5.1fMiB/s)\n", enc_clocks, 1.0 * enc_clocks / in_size, 1.0 * in_size / (enc_time * 1048576.0));
    }
    printf("interleaved rANS: %d bytes\n", (int) (out_buf + out_max_size - rans_begin));

    // try interleaved rANS decode
    for (int run=0; run < 5; run++) {
        double start_time = timer();
        uint64_t dec_start_time = get_timestamp_counter();

        RansState rans0, rans1;
        uint8_t* ptr = rans_begin;
        RansDecInit(&rans0, &ptr);
        RansDecInit(&rans1, &ptr);

        for (size_t i=0; i < (in_size & ~1); i += 2) {
            uint32_t s0 = cum2sym[RansDecGet(&rans0, prob_bits)];
            uint32_t s1 = cum2sym[RansDecGet(&rans1, prob_bits)];
            dec_bytes[i+0] = (uint8_t) s0;
            dec_bytes[i+1] = (uint8_t) s1;
            RansDecAdvanceSymbolStep(&rans0, &dsyms[s0], prob_bits);
            RansDecAdvanceSymbolStep(&rans1, &dsyms[s1], prob_bits);
            RansDecRenorm(&rans0, &ptr);
            RansDecRenorm(&rans1, &ptr);
        }

        // last byte, if number of bytes was odd
        if (in_size & 1) {
            uint32_t s0 = cum2sym[RansDecGet(&rans0, prob_bits)];
            dec_bytes[in_size - 1] = (uint8_t) s0;
            RansDecAdvanceSymbol(&rans0, &ptr, &dsyms[s0], prob_bits);
        }

        uint64_t dec_clocks = get_timestamp_counter() - dec_start_time;
        double dec_time = timer() - start_time;
        printf("  %"PRIu64" clocks, %.1f clocks/symbol (%5.1fMB/s)\n", dec_clocks, 1.0 * dec_clocks / in_size, 1.0 * in_size / (dec_time * 1048576.0));
    }

    // check decode results
    if (memcmp(in_bytes, dec_bytes, in_size) == 0)
        printf("decode ok!\n");
    else
        printf("ERROR: bad decoder!\n");
	
	printf("\n4-way interleaved rANS encode:\n");
    for (int run=0; run < 5; run++) {
        double start_time = timer();
        uint64_t enc_start_time = get_timestamp_counter();

        // odd number of bytes?
		RansState rans0, rans1, rans2, rans3;
		RansEncInit(&rans0);
		RansEncInit(&rans1);
		RansEncInit(&rans2);
		RansEncInit(&rans3);
		
		
		uint8_t* ptr = out_buf + out_max_size; // *end* of output buffer
		//printf("insize: %i, insize&3: %i\n ", in_size, in_size&3);
		switch (int i=(in_size&3)) {
		case 3: RansEncPutSymbol(&rans2, &ptr, &esyms[in_bytes[in_size-(i-2)]]);
		case 2: RansEncPutSymbol(&rans1, &ptr, &esyms[in_bytes[in_size-(i-1)]]);
		case 1: RansEncPutSymbol(&rans0, &ptr, &esyms[in_bytes[in_size-(i-0)]]);
		case 0:
		break;
		}

        for (size_t i=(in_size & ~3); i > 0; i -= 4) { // NB: working in reverse!
            int s3 = in_bytes[i-1];
            int s2 = in_bytes[i-2];           
			int s1 = in_bytes[i-3];
            int s0 = in_bytes[i-4];
            RansEncPutSymbol(&rans3, &ptr, &esyms[s3]);
            RansEncPutSymbol(&rans2, &ptr, &esyms[s2]);
			RansEncPutSymbol(&rans1, &ptr, &esyms[s1]);
            RansEncPutSymbol(&rans0, &ptr, &esyms[s0]);
        }
        RansEncFlush(&rans3, &ptr);
        RansEncFlush(&rans2, &ptr);
        RansEncFlush(&rans1, &ptr);
        RansEncFlush(&rans0, &ptr);
		
        rans_begin = ptr;

        uint64_t enc_clocks = get_timestamp_counter() - enc_start_time;
        double enc_time = timer() - start_time;
        printf("  %"PRIu64" clocks, %.1f clocks/symbol (%5.1fMiB/s)\n", enc_clocks, 1.0 * enc_clocks / in_size, 1.0 * in_size / (enc_time * 1048576.0));
    }
    printf("Size: %d bytes\n", (int) (out_buf + out_max_size - rans_begin));

    // try interleaved rANS decode
    for (int run=0; run < 5; run++) {
        double start_time = timer();
        uint64_t dec_start_time = get_timestamp_counter();

        uint8_t* ptr = rans_begin;
		
		RansState rans0, rans1, rans2, rans3;
        RansDecInit(&rans0, &ptr);
        RansDecInit(&rans1, &ptr);
        RansDecInit(&rans2, &ptr);
        RansDecInit(&rans3, &ptr);

		int out_end = (in_size&~3);
		for (int i=0; i < out_end; i+=4) {
            uint32_t s0 = cum2sym[RansDecGet(&rans0, prob_bits)];
            uint32_t s1 = cum2sym[RansDecGet(&rans1, prob_bits)];
            uint32_t s2 = cum2sym[RansDecGet(&rans2, prob_bits)];
            uint32_t s3 = cum2sym[RansDecGet(&rans3, prob_bits)];
            dec_bytes[i+0] = (uint8_t) s0;
            dec_bytes[i+1] = (uint8_t) s1;
            dec_bytes[i+2] = (uint8_t) s2;
            dec_bytes[i+3] = (uint8_t) s3;
            RansDecAdvanceSymbolStep(&rans0, &dsyms[s0], prob_bits);
            RansDecAdvanceSymbolStep(&rans1, &dsyms[s1], prob_bits);
            RansDecAdvanceSymbolStep(&rans2, &dsyms[s2], prob_bits);
            RansDecAdvanceSymbolStep(&rans3, &dsyms[s3], prob_bits);
            RansDecRenorm(&rans0, &ptr);
            RansDecRenorm(&rans1, &ptr);
            RansDecRenorm(&rans2, &ptr);
            RansDecRenorm(&rans3, &ptr);			
		}

		switch(in_size&3) 
		{
			case 3: 
			{
				uint32_t s2 = cum2sym[RansDecGet(&rans2, prob_bits)];
				dec_bytes[in_size+2] = (uint8_t) s2;
				RansDecAdvanceSymbolStep(&rans2, &dsyms[s2], prob_bits);
				RansDecRenorm(&rans2, &ptr);
			}
			case 2:
			{
				uint32_t s1 = cum2sym[RansDecGet(&rans1, prob_bits)];
				dec_bytes[in_size+1] = (uint8_t) s1;
				RansDecAdvanceSymbolStep(&rans1, &dsyms[s1], prob_bits);
				RansDecRenorm(&rans1, &ptr);
			}
			case 1:
			{
				uint32_t s0 = cum2sym[RansDecGet(&rans0, prob_bits)];
				dec_bytes[in_size+0] = (uint8_t) s0;
				RansDecAdvanceSymbolStep(&rans0, &dsyms[s0], prob_bits);
				RansDecRenorm(&rans0, &ptr);
			}
			default:
			break;
		}

        uint64_t dec_clocks = get_timestamp_counter() - dec_start_time;
        double dec_time = timer() - start_time;
        printf("  %"PRIu64" clocks, %.1f clocks/symbol (%5.1fMB/s)\n", dec_clocks, 1.0 * dec_clocks / in_size, 1.0 * in_size / (dec_time * 1048576.0));
    }

    if (memcmp(in_bytes, dec_bytes, in_size) == 0)
        printf("decode ok!\n");
    else
        printf("ERROR: bad decoder!\n");
	
	// ---- Implicit 4-way rANS encode/decode. Typical usage.

    memset(dec_bytes, 0xcc, in_size);

    printf("\n4-way implicit interleaved rANS encode:\n");

    for (int run=0; run < 5; run++) {
        double start_time = timer();
        uint64_t enc_start_time = get_timestamp_counter();

        RansState R[4];
        RansEncInit(&R[0]);
        RansEncInit(&R[1]);
        RansEncInit(&R[2]);
        RansEncInit(&R[3]);

        uint8_t* ptr = out_buf + out_max_size; // *end* of output buffer
        for (size_t i=in_size; i > 0; i--) { // NB: working in reverse!
			RansState X = R[3];
            int s = in_bytes[i-1];
            RansEncPutSymbol(&X, &ptr, &esyms[s]);
			R[3] = R[2];
			R[2] = R[1];
			R[1] = R[0];
			R[0] = X;
        }
        RansEncFlush(&R[3], &ptr);
        RansEncFlush(&R[2], &ptr);
        RansEncFlush(&R[1], &ptr);
        RansEncFlush(&R[0], &ptr);
        rans_begin = ptr;

        uint64_t enc_clocks = get_timestamp_counter() - enc_start_time;
        double enc_time = timer() - start_time;
        printf("  %"PRIu64" clocks, %.1f clocks/symbol (%5.1fMiB/s)\n", enc_clocks, 1.0 * enc_clocks / in_size, 1.0 * in_size / (enc_time * 1048576.0));
    }
    printf("Size: %d bytes\n", (int) (out_buf + out_max_size - rans_begin));

    // try rANS decode
    for (int run=0; run < 5; run++) {
        double start_time = timer();
        uint64_t dec_start_time = get_timestamp_counter();

        RansState R[4];
        uint8_t* ptr = rans_begin;
        RansDecInit(&R[0], &ptr);
        RansDecInit(&R[1], &ptr);
        RansDecInit(&R[2], &ptr);
        RansDecInit(&R[3], &ptr);

        for (size_t i=0; i < in_size; i++) 
		{
			RansState X = R[0];
			int range = RansDecGet(&X, prob_bits);
            uint32_t s = cum2sym[range];
            dec_bytes[i] = (uint8_t) s;
            RansDecAdvanceSymbol(&X, &ptr, &dsyms[s], prob_bits);
			
			R[0] = R[1];
			R[1] = R[2];
			R[2] = R[3];
			R[3] = X;
        }

        uint64_t dec_clocks = get_timestamp_counter() - dec_start_time;
        double dec_time = timer() - start_time;
        printf("  %"PRIu64" clocks, %.1f clocks/symbol (%5.1fMiB/s)\n", dec_clocks, 1.0 * dec_clocks / in_size, 1.0 * in_size / (dec_time * 1048576.0));
    }

    // check decode results
    if (memcmp(in_bytes, dec_bytes, in_size) == 0)
        printf("decode ok!\n");
    else
        printf("ERROR: bad decoder!\n");

    delete[] out_buf;
    delete[] dec_bytes;
    delete[] in_bytes;
    return 0;
}
