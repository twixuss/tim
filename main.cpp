#pragma warning(disable: 4996)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#undef assert

#define TL_USE_SIMD 1
#define TL_DEBUG 0
#define ENABLE_UNIT_TESTS 0
#define THREADED_ENCODING 0

#if !TL_DEBUG
#define bounds_check(...)
#endif

#define TL_IMPL
#include <tl/string.h>
#include <tl/file.h>
#include <tl/console.h>
#include <tl/precise_time.h>
#include <tl/main.h>
#include <tl/qoi.h>
#include <tl/thread.h>
#include <tl/cpu.h>

//#include "tim.h"

struct UnitTestRunner{
	int operator+(auto &&fn) {
		#if ENABLE_UNIT_TESTS
		fn();
		#endif
		return 0;
	}
};

#define UNIT_TEST int CONCAT(test, __LINE__) = UnitTestRunner{} + [&]()

using namespace tl;

using String = Span<utf8>;

inline constexpr bool operator==(String a, char const *b) { return a == as_utf8(as_span(b)); }

using Pixel = v3u8;

struct Image {
	Pixel *pixels = 0;
	v2u size = {};

	Pixel &operator()(s32 x, s32 y) {
		bounds_check(assert((u32)x < size.x && (u32)y <= size.y));
		return pixels[y*size.x + x];
	}
};

TaskQueueThreadPool thread_pool;

struct BitSerializer {
	uint8_t *buffer = 0;
	uint8_t *cursor = 0;
	uint8_t shift = 0;
	
	void skip(umm count) {
		cursor += count >> 3;
		shift += count & 7;
		cursor += shift >> 3;
		shift &= 7;
	}

	template <umm count>
	void write_bits(u64 value) {
		// k - keep
		// x - doesn't matter
		// 
		// e.g.
		//     count = 16
		//     shift = 3
		//     value = fedcba9876543210 (letter assigned to each bit)
		// 
		// cursor+0 cursor+1 cursor+2
		//
		// 43210kkk cba98765 xxxxxfed
	 
		// Shift the value and put previous data in low bits.
		value = (value << shift) | (*cursor & ((1 << shift) - 1));

		// Write to memory using minimum necessary size
			 if constexpr (count <=  1) *(u8  *)cursor = value;
		else if constexpr (count <=  9) *(u16 *)cursor = value;
		else if constexpr (count <= 25) *(u32 *)cursor = value;
		else if constexpr (count <= 57) *(u64 *)cursor = value;
		else static_error_v(count, "not supported. 58 or more bits might require a 9-byte write");

		// Advance
		shift += count;
		cursor += shift / 8;
		shift &= 7;
	}

	template <umm count>
	void write_bits_at(u64 offset, u64 value) {
		u8 *pointer = buffer + offset / 8;
		u64 shift = offset % 8;

		u64 mask = 0x8000000000000000;   // one bit at the top
		mask = (s64)mask >> (count - 1); // `count` bits at the top
		mask = mask >> (64 - count);     // `count` bits at the bottom
		mask <<= shift;
		mask = ~mask;

		value <<= shift;

		     if constexpr (count <=  1) *(u8  *)pointer = (*(u8  *)pointer & mask) | value;
		else if constexpr (count <=  9) *(u16 *)pointer = (*(u16 *)pointer & mask) | value;
		else if constexpr (count <= 25) *(u32 *)pointer = (*(u32 *)pointer & mask) | value;
		else if constexpr (count <= 57) *(u64 *)pointer = (*(u64 *)pointer & mask) | value;
		else static_error_v(count, "not supported. 58 or more bits might require a 9-byte write");
	}

	void write_bits(BitSerializer that) {
		umm size = that.current_bit_index();
		that.cursor = that.buffer;
		that.shift = 0;
		for (umm i = 0; i < size / 57; ++i) {
			write_bits<57>(that.read_bits<57>());
		}
		for (umm i = size / 57 * 57; i < size; ++i) {
			write_bits<1>(that.read_bits<1>());
		}
	}

	template <umm count>
	u64 read_bits() {
		// e.g.
		//     count = 16
		//     shift = 3
		//     value = fedcba9876543210 (letter assigned to each bit)
		// 
		// cursor+0 cursor+1 cursor+2
		//
		// 43210kkk cba98765 xxxxxfed

		// Read necessary bytes
		u64 result = 0;
			 if constexpr (count <=  1) result = *(u8  *)cursor;
		else if constexpr (count <=  9) result = *(u16 *)cursor;
		else if constexpr (count <= 25) result = *(u32 *)cursor;
		else if constexpr (count <= 57) result = *(u64 *)cursor;
		else static_error_v(count, "not supported. 58 or more bits might require a 9-byte read");

		// Shift and get rid of other bits
		result = (result >> shift) & ((1ull << count) - 1);
		
		// Advance
		shift += count;
		cursor += shift / 8;
		shift &= 7;

		return result;
	}
	template <umm count>
	u64 read_bits_at(u64 offset) {
		u8 *pointer = buffer + offset / 8;
		u64 shift = offset % 8;

		u64 mask = 0x8000000000000000;   // one bit at the top
		mask = (s64)mask >> (count - 1); // `count` bits at the top
		mask = mask >> (64 - count);     // `count` bits at the bottom

		u64 value;
		     if constexpr (count <=  1) value = (*(u8  *)pointer >> shift) & mask;
		else if constexpr (count <=  9) value = (*(u16 *)pointer >> shift) & mask;
		else if constexpr (count <= 25) value = (*(u32 *)pointer >> shift) & mask;
		else if constexpr (count <= 57) value = (*(u64 *)pointer >> shift) & mask;
		else static_error_v(count, "not supported. 58 or more bits might require a 9-byte write");

		return value;
	}

	
	template <umm count>
	s64 read_bits_and_sign_extend() {
		struct X {
			s64 value : count;
		};
		X x;
		x.value = read_bits<count>();
		return x.value;
	};

	Span<u8> span() {
		return {buffer, cursor + !!shift};
	}

	umm current_bit_index() {
		return (cursor - buffer) * 8 + shift;
	}

	void set_current_bit_index(umm index) {
		cursor = buffer + index / 8;
		shift = index & 7;
	}
};

umm uncompressed_size = 0;
umm n_bits0 = 0;
umm n_bits1 = 0;
umm n_bits2 = 0;
umm n_bits4 = 0;
umm n_bits8 = 0;

#define profile()                                                                    \
	auto timer = create_precise_timer();                                             \
	defer {                                                                          \
		auto time = elapsed_time(timer);                                             \
		println("{}ms / {}/s", time * 1000, format_bytes(uncompressed_size / time)); \
	};

#define TEST

// Shift Right Logical by Immediate bytes Accross Lanes (all 256 bits), while shifting in zeros
template <int shift>
forceinline __m256i shift_right(__m256i m) {
	// v u t s r q p o n m l k j i h g|f e d c b a 9 8 7 6 5 4 3 2 1 0
	//  >>>
	// X X X v u t s r q p o n m l k j|i h g f e d c b a 9 8 7 6 5 4 3


	__m128i s = _mm256_extracti128_si256(m, 1);
	// Extract high half to mix with lower half in the next step.
	// s = v u t s r q p o n m l k j i h g

	__m256i i = _mm256_srli_si256(m, shift);
	// this shifts within 128bit lanes:
	// i = X X X v u t s r q p o n m l k j|X X X f e d c b a 9 8 7 6 5 4 3

	// move "i h g" in the right place.
	s = _mm_slli_si128(s, 16 - shift);
	// s = i h g X X X X X X X X X X X X X

	return _mm256_or_si256(i, _mm256_inserti128_si256(_mm256_setzero_si256(), s, 0));
}
UNIT_TEST {
	__m256i a = _mm256_set_epi8(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31);
	a = shift_right<3>(a);
	assert(_mm256_extract_epi8(a, 0) == 28);
	assert(_mm256_extract_epi8(a, 1) == 27);
	assert(_mm256_extract_epi8(a, 2) == 26);
	assert(_mm256_extract_epi8(a, 3) == 25);
	assert(_mm256_extract_epi8(a, 4) == 24);
	assert(_mm256_extract_epi8(a, 5) == 23);
	assert(_mm256_extract_epi8(a, 6) == 22);
	assert(_mm256_extract_epi8(a, 7) == 21);
	assert(_mm256_extract_epi8(a, 8) == 20);
	assert(_mm256_extract_epi8(a, 9) == 19);
	assert(_mm256_extract_epi8(a, 10) == 18);
	assert(_mm256_extract_epi8(a, 11) == 17);
	assert(_mm256_extract_epi8(a, 12) == 16);
	assert(_mm256_extract_epi8(a, 13) == 15);
	assert(_mm256_extract_epi8(a, 14) == 14);
	assert(_mm256_extract_epi8(a, 15) == 13);
	assert(_mm256_extract_epi8(a, 16) == 12);
	assert(_mm256_extract_epi8(a, 17) == 11);
	assert(_mm256_extract_epi8(a, 18) == 10);
	assert(_mm256_extract_epi8(a, 19) == 9);
	assert(_mm256_extract_epi8(a, 20) == 8);
	assert(_mm256_extract_epi8(a, 21) == 7);
	assert(_mm256_extract_epi8(a, 22) == 6);
	assert(_mm256_extract_epi8(a, 23) == 5);
	assert(_mm256_extract_epi8(a, 24) == 4);
	assert(_mm256_extract_epi8(a, 25) == 3);
	assert(_mm256_extract_epi8(a, 26) == 2);
	assert(_mm256_extract_epi8(a, 27) == 1);
	assert(_mm256_extract_epi8(a, 28) == 0);
	assert(_mm256_extract_epi8(a, 29) == 0);
	assert(_mm256_extract_epi8(a, 30) == 0);
	assert(_mm256_extract_epi8(a, 31) == 0);
};

// Shift Right Logical by Immediate bytes Accross Lanes (all 256 bits), while shifting in low bytes from x
template <int shift>
forceinline __m256i shift_right(__m256i m, __m128i x) {
	// m = v u t s r q p o n m l k j i h g|f e d c b a 9 8 7 6 5 4 3 2 1 0
	// x = _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _|_ _ _ _ _ _ _ _ _ _ _ _ z y x w
	//      >>>
	//     y x w v u t s r q p o n m l k j|i h g f e d c b a 9 8 7 6 5 4 3


	__m128i s = _mm256_extracti128_si256(m, 1);
	// Extract high half to mix with lower half in the next step.
	// s = v u t s r q p o n m l k j i h g

	__m256i i = _mm256_srli_si256(m, shift);
	// this shifts within 128bit lanes:
	// i = X X X v u t s r q p o n m l k j|X X X f e d c b a 9 8 7 6 5 4 3

	// move "i h g" in the right place.
	s = _mm_slli_si128(s, 16 - shift);
	// s = i h g X X X X X X X X X X X X X

	m = _mm256_or_si256(i, _mm256_inserti128_si256(_mm256_setzero_si256(), s, 0));
	// m = _ _ _ v u t s r q p o n m l k j|i h g f e d c b a 9 8 7 6 5 4 3

	m = _mm256_or_si256(m, _mm256_inserti128_si256(_mm256_setzero_si256(), _mm_slli_si128(x, 16 - shift), 1));
	// m = y x w v u t s r q p o n m l k j|i h g f e d c b a 9 8 7 6 5 4 3

	return m;
}

// Pack together 4 regions of 3 bytes located on every 6th byte
__m128i pack4_6to3(__m256i m) {
	// ? ? ? ? ? ? ? ? ? ? ? b a 9 ? ?|? 8 7 6 ? ? ? 5 4 3 ? ? ? 2 1 0
	//  >>>
	// ? ? ? ? ? ? ? ? ? ? ? ? ? ? ? ?|? ? ? ? b a 9 8 7 6 5 4 3 2 1 0

	__m128i l = _mm256_extracti128_si256(m, 0); // ? 8 7 6 ? ? ? 5 4 3 ? ? ? 2 1 0
	__m128i h = _mm256_extracti128_si256(m, 1); // ? ? ? ? ? ? ? ? ? ? ? b a 9 ? ?

	l = _mm_shuffle_epi8(l, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1, 14,13,12,8,7,6,2,1,0));    // X X X X X X X 8 7 6 5 4 3 2 1 0
	h = _mm_shuffle_epi8(h, _mm_set_epi8(-1,-1,-1,-1, 4,3,2,-1,-1,-1,-1,-1,-1,-1,-1,-1)); // X X X X b a 9 X X X X X X X X X

	return _mm_or_si128(l, h);
}
UNIT_TEST {
	__m256i a = _mm256_setr_epi8(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31);
	__m128i b = pack4_6to3(a);
	assert(_mm_extract_epi8(b, 0) == 0);
	assert(_mm_extract_epi8(b, 1) == 1);
	assert(_mm_extract_epi8(b, 2) == 2);
	assert(_mm_extract_epi8(b, 3) == 6);
	assert(_mm_extract_epi8(b, 4) == 7);
	assert(_mm_extract_epi8(b, 5) == 8);
	assert(_mm_extract_epi8(b, 6) == 12);
	assert(_mm_extract_epi8(b, 7) == 13);
	assert(_mm_extract_epi8(b, 8) == 14);
	assert(_mm_extract_epi8(b, 9) == 18);
	assert(_mm_extract_epi8(b, 10) == 19);
	assert(_mm_extract_epi8(b, 11) == 20);
};

forceinline __m128i cvtepu16_epu8(__m256i m) {
	// v u t s r q p o n m l k j i h g|f e d c b a 9 8 7 6 5 4 3 2 1 0
	//  >>>
	//                                 u s q o m j i g e c a 8 6 4 2 0
	
	__m128i l = _mm256_extracti128_si256(m, 0); // f e d c b a 9 8 7 6 5 4 3 2 1 0
	__m128i h = _mm256_extracti128_si256(m, 1); // v u t s r q p o n m l k j i h g

	l = _mm_shuffle_epi8(l, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1,14,12,10,8,6,4,2,0)); // _ _ _ _ _ _ _ _ e c a 8 6 4 2 0
	h = _mm_shuffle_epi8(h, _mm_set_epi8(14,12,10,8,6,4,2,0,-1,-1,-1,-1,-1,-1,-1,-1)); // u s q o m j i g _ _ _ _ _ _ _ _

	return _mm_or_si128(l, h);
}
UNIT_TEST {
	__m256i a = _mm256_setr_epi8(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31);
	__m128i b = cvtepu16_epu8(a);
	assert(_mm_extract_epi8(b, 0) == 0);
	assert(_mm_extract_epi8(b, 1) == 2);
	assert(_mm_extract_epi8(b, 2) == 4);
	assert(_mm_extract_epi8(b, 3) == 6);
	assert(_mm_extract_epi8(b, 4) == 8);
	assert(_mm_extract_epi8(b, 5) == 10);
	assert(_mm_extract_epi8(b, 6) == 12);
	assert(_mm_extract_epi8(b, 7) == 14);
	assert(_mm_extract_epi8(b, 8) == 16);
	assert(_mm_extract_epi8(b, 9) == 18);
	assert(_mm_extract_epi8(b, 10) == 20);
	assert(_mm_extract_epi8(b, 11) == 22);
	assert(_mm_extract_epi8(b, 12) == 24);
	assert(_mm_extract_epi8(b, 13) == 26);
	assert(_mm_extract_epi8(b, 14) == 28);
	assert(_mm_extract_epi8(b, 15) == 30);
};

template <int n>
__m128i shift_right_epi8(__m128i m) {
	// 76543210 76543210 76543210 76543210
	//   >> 3
	// 00076543 00076543 00076543 00076543

	return _mm_and_si128(_mm_srli_epi16(m, n), _mm_set1_epi16(~(-256 >> n) | 0xff00));
}
template <int n>
__m256i shift_right_epi8(__m256i m) {
	return _mm256_and_si256(_mm256_srli_epi16(m, n), _mm256_set1_epi16(~(-256 >> n) | 0xff00));
}
UNIT_TEST {
	__m128i a = _mm_setr_epi8(192,128,96,64,48,32,24,16,12,8,6,4,3,2,1,0);
	a = shift_right_epi8<2>(a);
	
	assert(_mm_extract_epi8(a, 0)  == 192 >> 2);
	assert(_mm_extract_epi8(a, 1)  == 128 >> 2);
	assert(_mm_extract_epi8(a, 2)  == 96  >> 2);
	assert(_mm_extract_epi8(a, 3)  == 64  >> 2);
	assert(_mm_extract_epi8(a, 4)  == 48  >> 2);
	assert(_mm_extract_epi8(a, 5)  == 32  >> 2);
	assert(_mm_extract_epi8(a, 6)  == 24  >> 2);
	assert(_mm_extract_epi8(a, 7)  == 16  >> 2);
	assert(_mm_extract_epi8(a, 8)  == 12  >> 2);
	assert(_mm_extract_epi8(a, 9)  == 8   >> 2);
	assert(_mm_extract_epi8(a, 10) == 6   >> 2);
	assert(_mm_extract_epi8(a, 11) == 4   >> 2);
	assert(_mm_extract_epi8(a, 12) == 3   >> 2);
	assert(_mm_extract_epi8(a, 13) == 2   >> 2);
	assert(_mm_extract_epi8(a, 14) == 1   >> 2);
	assert(_mm_extract_epi8(a, 15) == 0   >> 2);
};

forceinline __m128i count_leading_zeros_epi8(__m128i x) {
	__m128i n = _mm_set1_epi8(8);
	__m128i y;
	__m128i m;

	y = shift_right_epi8<4>(x); m = _mm_cmpeq_epi16(y, _mm_setzero_si128()); n = _mm_blendv_epi8(_mm_sub_epi8(n, _mm_set1_epi8(4)), n, m); x = _mm_blendv_epi8(y, x, m);
	y = shift_right_epi8<2>(x); m = _mm_cmpeq_epi16(y, _mm_setzero_si128()); n = _mm_blendv_epi8(_mm_sub_epi8(n, _mm_set1_epi8(2)), n, m); x = _mm_blendv_epi8(y, x, m);
	y = shift_right_epi8<1>(x); m = _mm_cmpeq_epi16(y, _mm_setzero_si128()); n = _mm_blendv_epi8(_mm_sub_epi8(n, _mm_set1_epi8(1)), n, m); x = _mm_blendv_epi8(y, x, m);
	return _mm_sub_epi8(n, x);
}
forceinline __m256i count_leading_zeros_epi8(__m256i x) {
	__m256i n = _mm256_set1_epi8(8);
	__m256i y;
	__m256i m;

	y = shift_right_epi8<4>(x); m = _mm256_cmpeq_epi16(y, _mm256_setzero_si256()); n = _mm256_blendv_epi8(_mm256_sub_epi8(n, _mm256_set1_epi8(4)), n, m); x = _mm256_blendv_epi8(y, x, m);
	y = shift_right_epi8<2>(x); m = _mm256_cmpeq_epi16(y, _mm256_setzero_si256()); n = _mm256_blendv_epi8(_mm256_sub_epi8(n, _mm256_set1_epi8(2)), n, m); x = _mm256_blendv_epi8(y, x, m);
	y = shift_right_epi8<1>(x); m = _mm256_cmpeq_epi16(y, _mm256_setzero_si256()); n = _mm256_blendv_epi8(_mm256_sub_epi8(n, _mm256_set1_epi8(1)), n, m); x = _mm256_blendv_epi8(y, x, m);
	return _mm256_sub_epi8(n, x);
}
UNIT_TEST {
	__m128i a = _mm_setr_epi8(0,1,2,4,8,16,32,64,128,0,0,0,0,0,0,0);
	a = count_leading_zeros_epi8(a);
	
	assert(_mm_extract_epi8(a, 0) == 8);
	assert(_mm_extract_epi8(a, 1) == 7);
	assert(_mm_extract_epi8(a, 2) == 6);
	assert(_mm_extract_epi8(a, 3) == 5);
	assert(_mm_extract_epi8(a, 4) == 4);
	assert(_mm_extract_epi8(a, 5) == 3);
	assert(_mm_extract_epi8(a, 6) == 2);
	assert(_mm_extract_epi8(a, 7) == 1);
	assert(_mm_extract_epi8(a, 8) == 0);
};

void compute_mids_impl_scalar(Image image, Pixel *mids) {
	//
	// Non simd. One component at a time
	// 
	// total speed: ~85mb/s
	//
	for (int component = 0; component < 3; ++component) {
		for (umm y = 0; y < image.size.y / 2; ++y) {
		for (umm x = 0; x < image.size.x / 2; ++x) {
			u8 mn = 255;
			u8 mx = 0;
			for (umm dy = 0; dy < 2; ++dy) {
			for (umm dx = 0; dx < 2; ++dx) {
				auto p = image(x*2 + dx, y*2 + dy).s[component];
				mn = min(mn, p);
				mx = max(mx, p);
			}
			}

			mids[y*image.size.x/2 + x].s[component] = (u8)(((u16)mn + (u16)mx + 1) / 2);
		}
		}
	}
}
void compute_mids_impl_sse41(Image image, Pixel *mids) {
	//
	// uses _mm_min_epu8 / _mm_max_epu8 and shuffling
	// works on all 3 components in parallel. two quads at once
	//
	// total speed: ~103mb/s
	//
	__m128i wu16_to_u8 = _mm_setr_epi8(0,2,4,6,8,10,12,14,-1,-1,-1,-1,-1,-1,-1,-1);
	__m128i wone16 = _mm_set1_epi16(1);
	__m128i wpack = _mm_setr_epi8(0,1,2,6,7,8,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1);
	for (umm y = 0; y < image.size.y / 2; ++y) {
	for (umm x = 0; x < image.size.x / 2; x += 2) {
		auto r1 = _mm_loadu_si128((__m128i *)(image.pixels + (y*2 + 0)*image.size.x + (x*2 + 0)));
		auto r2 = _mm_loadu_si128((__m128i *)(image.pixels + (y*2 + 1)*image.size.x + (x*2 + 0)));
		//      0     1     2     3     4     5     6     7     8     9     10    11    12 13 14 15
		// r1 = p1.x, p1.y, p1.z, p2.x, p2.y, p2.z, p5.x, p5.y, p5.z, p6.x, p6.y, p6.z, ?, ?, ?, ?
		// r2 = p3.x, p3.y, p3.z, p4.x, p4.y, p4.z, p7.x, p7.y, p7.z, p8.x, p8.y, p8.z, ?, ?, ?, ?

		__m128i wmn = _mm_min_epu8(r1, r2);
		__m128i wmx = _mm_max_epu8(r1, r2);
		wmn = _mm_min_epu8(wmn, _mm_srli_si128(wmn, 3));
		wmx = _mm_max_epu8(wmx, _mm_srli_si128(wmx, 3));
		//       0      1      2      3  4  5  6      7      8      9  10 11 12 13 14 15
		// wmn = mn1.x, mn1.y, mn1.z, ?, ?, ?, mn2.x, mn2.y, mn2.z, ?, ?, ?, ?, ?, ?, ?

		wmn = _mm_shuffle_epi8(wmn, wpack);
		wmx = _mm_shuffle_epi8(wmx, wpack);
		//       0      1      2      3      4      5      6  7  8  9  10 11 12 13 14 15
		// wmn = mn1.x, mn1.y, mn1.z, mn2.x, mn2.y, mn2.z, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?
		
		wmn = _mm_cvtepu8_epi16(wmn);
		wmx = _mm_cvtepu8_epi16(wmx);
		//       0      1      2      3      4      5      6  7
		// wmn = mn1.x, mn1.y, mn1.z, mn2.x, mn2.y, mn2.z, ?, ?
		
		__m128i wm = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(wmn, wmx), wone16), 1); // (mn + mx + 1) / 2
		wm = _mm_shuffle_epi8(wm, wu16_to_u8); // cvtepu16_epi8 is avx512...

		#if 1
		// writes only what's necessary.
		memcpy(&mids[y*image.size.x/2 + x], &wm, 6);
		#else
		// writes 8 bytes instead of 6, but requires less instructions. Speed difference is ~0
		*(u64 *)&mids[y*image.size.x/2 + x] = _mm_extract_epi64(wm, 0);
		#endif
	}
	}
}
void compute_mids_impl_avx2(Image image, Pixel *mids) {
	#if 1
	//
	// uses _mm256_min_epu8 / _mm256_max_epu8 and shuffling
	// works on all 3 components in parallel, four quads.
	//
	__m256i wone16 = _mm256_set1_epi16(1);
	for (umm y = 0; y < image.size.y / 2; ++y) {
	for (umm x = 0; x < image.size.x / 2; x += 4) {
		auto r1 = _mm256_loadu_si256((__m256i *)(image.pixels + (y*2 + 0)*image.size.x + (x*2 + 0)));
		auto r2 = _mm256_loadu_si256((__m256i *)(image.pixels + (y*2 + 1)*image.size.x + (x*2 + 0)));
		//      0     1     2     3     4     5     6     7     8     9     10    11    12     13     14     15     16     17     18     19     20     21     22     23     24 25 26 27 28 29 30 31
		// r1 = p1.x, p1.y, p1.z, p2.x, p2.y, p2.z, p5.x, p5.y, p5.z, p6.x, p6.y, p6.z, p9.x,  p9.y,  p9.z,  p10.x, p10.y, p10.z, p13.x, p13.y, p13.z, p14.x, p14.y, p14.z, ?, ?, ?, ?, ?, ?, ?, ?
		// r2 = p3.x, p3.y, p3.z, p4.x, p4.y, p4.z, p7.x, p7.y, p7.z, p8.x, p8.y, p8.z, p11.x, p11.y, p11.z, p12.x, p12.y, p12.z, p15.x, p15.y, p15.z, p16.x, p16.y, p16.z, ?, ?, ?, ?, ?, ?, ?, ?
		
		__m256i wmn = _mm256_min_epu8(r1, r2);
		__m256i wmx = _mm256_max_epu8(r1, r2);
		
		wmn = _mm256_min_epu8(wmn, shift_right<3>(wmn));
		wmx = _mm256_max_epu8(wmx, shift_right<3>(wmx));
		//       0      1      2      3  4  5  6      7      8      9  10 11 12     13     14     15 16 17 18     19     20     21 22 23 24 25 26 27 28 29 30 31
		// wmn = mn1.x, mn1.y, mn1.z, ?, ?, ?, mn2.x, mn2.y, mn2.z, ?, ?, ?, mn3.x, mn3.y, mn3.z, ?, ?, ?, mn4.x, mn4.y, mn4.z, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?

		__m128i wmnp = pack4_6to3(wmn);
		__m128i wmxp = pack4_6to3(wmx);
		//        0      1      2      3      4      5      6      7      8      9      10     11     12 13 14 15
		// wmnp = mn1.x, mn1.y, mn1.z, mn2.x, mn2.y, mn2.z, mn3.x, mn3.y, mn3.z, mn4.x, mn4.y, mn4.z, ?, ?, ?, ?
		
		wmn = _mm256_cvtepu8_epi16(wmnp);
		wmx = _mm256_cvtepu8_epi16(wmxp);
		//       0      1      2      3      4      5      6  7
		// wmn = mn1.x, mn1.y, mn1.z, mn2.x, mn2.y, mn2.z, ?, ?
		
		__m256i wm16 = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(wmn, wmx), wone16), 1); // (mn + mx + 1) / 2
		__m128i wm = cvtepu16_epu8(wm16); // cvtepu16_epi8 is avx512...
		
		memcpy(&mids[y*image.size.x/2 + x], &wm, 12);
	}
	}
	#else
	//
	// uses _mm256_min_epu8 / _mm256_max_epu8 and shuffling
	// works on all 3 components in parallel, sixteen quads.
	//
	// Extremely unrolled version. It uses almost all 16 simd registers.
	// But 
	//
	__m256i wone16 = _mm256_set1_epi16(1);
	for (umm y = 0; y < image.size.y / 2; ++y) {
	for (umm x = 0; x < image.size.x / 2; x += 4) {
		auto r1p = (__m256i *)(image.pixels + (y*2 + 0)*image.size.x + (x*2 + 0));
		auto r2p = (__m256i *)(image.pixels + (y*2 + 1)*image.size.x + (x*2 + 0));
		auto r11 = _mm256_loadu_si256(r1p + 0);
		auto r12 = _mm256_loadu_si256(r1p + 1);
		auto r13 = _mm256_loadu_si256(r1p + 2);
		auto r21 = _mm256_loadu_si256(r2p + 0);
		auto r22 = _mm256_loadu_si256(r2p + 1);
		auto r23 = _mm256_loadu_si256(r2p + 2);
		//      0     1     2     3     4     5     6     7     8     9     10    11    12     13     14     15     16     17     18     19     20     21     22     23     24 25 26 27 28 29 30 31
		// r1 = p1.x, p1.y, p1.z, p2.x, p2.y, p2.z, p5.x, p5.y, p5.z, p6.x, p6.y, p6.z, p9.x,  p9.y,  p9.z,  p10.x, p10.y, p10.z, p13.x, p13.y, p13.z, p14.x, p14.y, p14.z, ?, ?, ?, ?, ?, ?, ?, ?
		// r2 = p3.x, p3.y, p3.z, p4.x, p4.y, p4.z, p7.x, p7.y, p7.z, p8.x, p8.y, p8.z, p11.x, p11.y, p11.z, p12.x, p12.y, p12.z, p15.x, p15.y, p15.z, p16.x, p16.y, p16.z, ?, ?, ?, ?, ?, ?, ?, ?
		
		__m256i wmn1 = _mm256_min_epu8(r11, r21);
		__m256i wmn2 = _mm256_min_epu8(r12, r22);
		__m256i wmn3 = _mm256_min_epu8(r13, r23);
		__m256i wmx1 = _mm256_max_epu8(r11, r21);
		__m256i wmx2 = _mm256_max_epu8(r12, r22);
		__m256i wmx3 = _mm256_max_epu8(r13, r23);
		
		// vutsrqponmlkjihgfedcba9876543210 vutsrqponmlkjihgfedcba9876543210 vutsrqponmlkjihgfedcba9876543210
		// ___vutsrqponmlkjihgfedcba9876543 210vutsrqponmlkjihgfedcba9876543 210vutsrqponmlkjihgfedcba9876543
		
		wmn1 = _mm256_min_epu8(wmn1, shift_right<3>(wmn1, _mm256_extracti128_si256(wmn2, 0)));
		wmn2 = _mm256_min_epu8(wmn2, shift_right<3>(wmn2, _mm256_extracti128_si256(wmn3, 0)));
		wmn3 = _mm256_min_epu8(wmn3, shift_right<3>(wmn3));
		wmx1 = _mm256_max_epu8(wmx1, shift_right<3>(wmx1, _mm256_extracti128_si256(wmx2, 0)));
		wmx2 = _mm256_max_epu8(wmx2, shift_right<3>(wmx2, _mm256_extracti128_si256(wmx3, 0)));
		wmx3 = _mm256_max_epu8(wmx3, shift_right<3>(wmx3));
		//        vutsrqponmlkjihg|fedcba9876543210
		// wmn1 = yx???zyx???zyx??|?zyx???zyx???zyx
		// wmn2 = ?zyx???zyx???zyx|???zyx???zyx???z
		// wmn3 = ???zyx???zyx???z|yx???zyx???zyx??
		
		__m128i wmn1l = _mm256_extracti128_si256(wmn1, 0);
		__m128i wmn1h = _mm256_extracti128_si256(wmn1, 1);
		__m128i wmn2l = _mm256_extracti128_si256(wmn2, 0);
		__m128i wmn2h = _mm256_extracti128_si256(wmn2, 1);
		__m128i wmn3l = _mm256_extracti128_si256(wmn3, 0);
		__m128i wmn3h = _mm256_extracti128_si256(wmn3, 1);

		__m128i wmx1l = _mm256_extracti128_si256(wmx1, 0);
		__m128i wmx1h = _mm256_extracti128_si256(wmx1, 1);
		__m128i wmx2l = _mm256_extracti128_si256(wmx2, 0);
		__m128i wmx2h = _mm256_extracti128_si256(wmx2, 1);
		__m128i wmx3l = _mm256_extracti128_si256(wmx3, 0);
		__m128i wmx3h = _mm256_extracti128_si256(wmx3, 1);
		
		__m128i mn1 = _mm_or_si128(
			_mm_shuffle_epi8(wmn1l, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,14,13,12, 8, 7, 6, 2, 1, 0)),
			_mm_shuffle_epi8(wmn1h, _mm_set_epi8(14,10, 9, 8, 4, 3, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1))
		);
		__m128i mn2 = _mm_or_si128(_mm_or_si128(
			_mm_shuffle_epi8(wmn1h, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,15)),
			_mm_shuffle_epi8(wmn2l, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1,12,11,10, 6, 5, 4, 0,-1))),
			_mm_shuffle_epi8(wmn2h, _mm_set_epi8(13,12, 8, 7, 6, 2, 1, 0,-1,-1,-1,-1,-1,-1,-1,-1))
		);
		__m128i mn3 = _mm_or_si128(_mm_or_si128(
			_mm_shuffle_epi8(wmn2h, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,14)),
			_mm_shuffle_epi8(wmn3l, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,15,14,10, 9, 8, 4, 3, 2,-1))),
			_mm_shuffle_epi8(wmn3h, _mm_set_epi8(12,11,10, 6, 5, 4, 0,-1,-1,-1,-1,-1,-1,-1,-1,-1))
		);
		__m128i mx1 = _mm_or_si128(
			_mm_shuffle_epi8(wmx1l, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,14,13,12, 8, 7, 6, 2, 1, 0)),
			_mm_shuffle_epi8(wmx1h, _mm_set_epi8(14,10, 9, 8, 4, 3, 2,-1,-1,-1,-1,-1,-1,-1,-1,-1))
		);
		__m128i mx2 = _mm_or_si128(_mm_or_si128(
			_mm_shuffle_epi8(wmx1h, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,15)),
			_mm_shuffle_epi8(wmx2l, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1,12,11,10, 6, 5, 4, 0,-1))),
			_mm_shuffle_epi8(wmx2h, _mm_set_epi8(13,12, 8, 7, 6, 2, 1, 0,-1,-1,-1,-1,-1,-1,-1,-1))
		);
		__m128i mx3 = _mm_or_si128(_mm_or_si128(
			_mm_shuffle_epi8(wmx2h, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,14)),
			_mm_shuffle_epi8(wmx3l, _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,15,14,10, 9, 8, 4, 3, 2,-1))),
			_mm_shuffle_epi8(wmx3h, _mm_set_epi8(12,11,10, 6, 5, 4, 0,-1,-1,-1,-1,-1,-1,-1,-1,-1))
		);
		
		wmn1 = _mm256_cvtepu8_epi16(mn1);
		wmn2 = _mm256_cvtepu8_epi16(mn2);
		wmn3 = _mm256_cvtepu8_epi16(mn3);
		wmx1 = _mm256_cvtepu8_epi16(mx1);
		wmx2 = _mm256_cvtepu8_epi16(mx2);
		wmx3 = _mm256_cvtepu8_epi16(mx3);

		 // (mn + mx + 1) / 2
		__m256i wm1 = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(wmn1, wmx1), wone16), 1);
		__m256i wm2 = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(wmn2, wmx2), wone16), 1);
		__m256i wm3 = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(wmn3, wmx3), wone16), 1);
		
		// _mm_cvtepi16_epi8 is avx512...
		__m128i wmb1 = cvtepu16_epu8(wm1);
		__m128i wmb2 = cvtepu16_epu8(wm2);
		__m128i wmb3 = cvtepu16_epu8(wm3);
		
		memcpy((u8 *)&mids[y * image.size.x / 2 + x] +  0, &wmb1, 16);
		memcpy((u8 *)&mids[y * image.size.x / 2 + x] + 16, &wmb2, 16);
		memcpy((u8 *)&mids[y * image.size.x / 2 + x] + 32, &wmb3, 16);
	}
	}
	#endif
}
Pixel *compute_mids(Image image) {
	Pixel *mids = DefaultAllocator{}.allocate<Pixel>(image.size.x * image.size.y / (2*2) + 64);

	static void (*compute_mids_impl)(Image image, Pixel *mids) = 0;

	if (!compute_mids_impl) [[unlikely]] {
		auto cpu = get_cpu_info();
		if (cpu.has_feature(CpuFeature_avx2)) {
			compute_mids_impl = compute_mids_impl_avx2;
		} else if (cpu.has_feature(CpuFeature_sse41)) {
			compute_mids_impl = compute_mids_impl_sse41;
		} else {
			compute_mids_impl = compute_mids_impl_scalar;
		}
	}

	compute_mids_impl(image, mids);

	return mids;
}

umm allocate_row_offsets(BitSerializer &serializer, Image image) {
	auto pointer = serializer.current_bit_index();
	// image.size.y/2 rows for every component plus one extra offset to skip everything
	serializer.skip((image.size.y / 2 * 3 + 1) * 32);
	return pointer;
}

void write_deltas(BitSerializer &serializer, Image image, Pixel *mids, umm row_offsets_pointer, int mip_level) {
	u64 avg_bits_required = 0;
	u64 avg_bits_required_div = 0;
	
	#if THREADED_ENCODING
	List<BitSerializer> serializers;
	defer {
		for (auto &serializer : serializers) {
			DefaultAllocator{}.free(serializer.buffer);
		}
		free(serializers);
	};
	
	serializers.resize(image.size.y / 2 * 3);
	for (auto &serializer : serializers) {
		serializer.buffer = serializer.cursor = DefaultAllocator{}.allocate<u8>(image.size.x / 2 * 3 * 2);
	}
	#endif

	for (int component = 0; component < 3; ++component) {
		for (smm y = 0; y < image.size.y / 2; ++y) {
			#if !THREADED_ENCODING
			// Write the pointer to the beggining of this row in previously allocated table.
			serializer.write_bits_at<32>(row_offsets_pointer + (component*image.size.y/2 + y)*32, serializer.current_bit_index());
			#endif

			#if THREADED_ENCODING
			thread_pool += [=, &serializer = serializers[component*image.size.y/2 + y]]() mutable {
			#endif
				for (smm x = 0; x < image.size.x / 2; ++x) {
					u8 mid = mids[y*image.size.x/2 + x].s[component];
					
					s8 deltas[4];
					deltas[0] = image(x*2 + 0, y*2 + 0).s[component] - mid;
					deltas[1] = image(x*2 + 1, y*2 + 0).s[component] - mid;
					deltas[2] = image(x*2 + 0, y*2 + 1).s[component] - mid;
					deltas[3] = image(x*2 + 1, y*2 + 1).s[component] - mid;

					#if 1
					__m128i d = _mm_setzero_si128();
					d = _mm_insert_epi16(d, deltas[0], 0);
					d = _mm_insert_epi16(d, deltas[1], 1);
					d = _mm_insert_epi16(d, deltas[2], 2);
					d = _mm_insert_epi16(d, deltas[3], 3);

					d = _mm_xor_si128(d, _mm_set1_epi16(0x8000));

					d = _mm_insert_epi64(d, -1, 1);
					s8 dmin = _mm_extract_epi16(_mm_minpos_epu16(d), 0);
					d = _mm_insert_epi64(_mm_xor_si128(d, _mm_set1_epi8(-1)), -1, 1);
					s8 dmax = ~_mm_extract_epi16(_mm_minpos_epu16(d), 0);

					#else

					#if 0
					// ~130mb/s
					s8 dmin = min(deltas[0], deltas[1], deltas[2], deltas[3]);
					s8 dmax = max(deltas[0], deltas[1], deltas[2], deltas[3]);
					#else
					// ~150mb/s ???
					s8 dmin = 127;
					s8 dmax = -128;
					for (auto delta : deltas) {
						dmin = min(dmin, delta);
						dmax = max(dmax, delta);
					}
					#endif
					#endif

					#if 1

					u8 table[] = {1,2,4,4,8,8,8,8,8,8,8,8};
					u8 bits_required = table[8 - count_leading_zeros(max((s8)-(dmin+1), dmax))];
					bits_required = ((0 == dmin) & (dmax == 0)) ? 0 : bits_required;

					#else

					u8 bits_required = 0;
					if (0 == dmin && dmax == 0) {
						// no bits required
					} else if (-1 <= dmin && dmax < 1) {
						bits_required = max(bits_required, (u8)1);
					} else if (-2 <= dmin && dmax < 2) {
						bits_required = max(bits_required, (u8)2);
					} else if (-8 <= dmin && dmax < 8) {
						bits_required = max(bits_required, (u8)4);
					} else {
						bits_required = max(bits_required, (u8)8);
					}

					#endif

					atomic_add(&avg_bits_required, bits_required);
					atomic_add(&avg_bits_required_div, 1);

					// NOTE: Least significant bit is written first.
					//       If read one by one, these literals will read right to left by decoder.
					switch (bits_required) {
						case 0: {
							++n_bits0;
							serializer.write_bits<1>(0b0);
							break;
						}
						case 1: {
							++n_bits1;
							u64 data = 0b001
								| ((deltas[0] & 1) << (3+1*0))
								| ((deltas[1] & 1) << (3+1*1))
								| ((deltas[2] & 1) << (3+1*2))
								| ((deltas[3] & 1) << (3+1*3));
							serializer.write_bits<3 + 4*1>(data);
							break;
						}
						case 2: {
							++n_bits2;
							u64 data = 0b101
								| ((deltas[0] & 3) << (3+2*0))
								| ((deltas[1] & 3) << (3+2*1))
								| ((deltas[2] & 3) << (3+2*2))
								| ((deltas[3] & 3) << (3+2*3));
							serializer.write_bits<3 + 4*2>(data);
							break;
						}
						case 4: {
							++n_bits4;
							u64 data = 0b011
								| ((deltas[0] & 15) << (3+4*0))
								| ((deltas[1] & 15) << (3+4*1))
								| ((deltas[2] & 15) << (3+4*2))
								| ((deltas[3] & 15) << (3+4*3));
							serializer.write_bits<3 + 4*4>(data);
							break;
						}
						case 8: {
							++n_bits8;
							u64 data = 0b111
								| ((u64)(u8)deltas[0] << (3+8*0))
								| ((u64)(u8)deltas[1] << (3+8*1))
								| ((u64)(u8)deltas[2] << (3+8*2))
								| ((u64)(u8)deltas[3] << (3+8*3));
							serializer.write_bits<3 + 4*8>(data);
							break;
						}
					}
				}
			#if THREADED_ENCODING
			};
			#endif
		}
	}
	
	#if THREADED_ENCODING
	thread_pool.wait_for_completion(WaitForCompletionOption::do_any_task);
	
	umm i = 0;
	for (auto &thread_serializer : serializers) {
		// Write the pointer to the beggining of this row in previously allocated table.
		serializer.write_bits_at<32>(row_offsets_pointer + i++ * 32, serializer.current_bit_index());

		serializer.write_bits(thread_serializer);
	}
	#endif

	serializer.write_bits_at<32>(row_offsets_pointer + (3*image.size.y/2)*32, serializer.current_bit_index());

	//println("{}: avg bits: {}", mip_level, (f64)avg_bits_required / avg_bits_required_div);
}

void encode_mip(BitSerializer &serializer, Image image, int mip_level = 0) {
	Pixel *mids = compute_mids(image);
	defer { DefaultAllocator{}.free(mids); };

	if (image.size.x == 2) {
		serializer.write_bits<8>(mids[0].x);
		serializer.write_bits<8>(mids[0].y);
		serializer.write_bits<8>(mids[0].z);
	} else {
		encode_mip(serializer, {mids, image.size / 2}, mip_level + 1);
	}

	auto row_offsets_pointer = allocate_row_offsets(serializer, image);

	write_deltas(serializer, image, mids, row_offsets_pointer, mip_level);
}

void encode(BitSerializer &serializer, Image in, v2u unpadded_size) {
	profile();

	serializer.write_bits<32>(unpadded_size.x);
	serializer.write_bits<32>(unpadded_size.y);

	encode_mip(serializer, in);
}

Image pad_to_power_of_2(Image image) {
	umm padded_dim = ceil_to_power_of_2(max(image.size));

	Pixel *new_pixels = DefaultAllocator{}.allocate<Pixel>(padded_dim * padded_dim);

	for (umm y = 0; y < image.size.y; ++y) {
		memcpy(new_pixels + y*padded_dim, image.pixels + y*image.size.x, image.size.x * sizeof(Pixel));
	}
	
	image.size = V2u(padded_dim);
	image.pixels = new_pixels;
	return image;
}

Image decode(BitSerializer &serializer) {
	v2u unpadded_size;
	unpadded_size.x = serializer.read_bits<32>();
	unpadded_size.y = serializer.read_bits<32>();


	Image out = {};
	out.size = unpadded_size;

	bool is_p2_square = unpadded_size.x == unpadded_size.y && is_power_of_2(unpadded_size.x);

	u32 padded_dim = ceil_to_power_of_2(max(unpadded_size));

	uncompressed_size = unpadded_size.x * unpadded_size.y * sizeof(Pixel);

	profile();

	auto buffer_in  = DefaultAllocator{}.allocate<Pixel>(padded_dim * padded_dim + 64);
	auto buffer_out = DefaultAllocator{}.allocate<Pixel>(padded_dim * padded_dim + 64);

	buffer_out[0].s[0] = serializer.read_bits<8>();
	buffer_out[0].s[1] = serializer.read_bits<8>();
	buffer_out[0].s[2] = serializer.read_bits<8>();
	
	umm mip_count = log(padded_dim, 2) + 1;
	for (umm i = 1; i < mip_count; ++i) {
		auto mip_size = V2u(::pow(2, i));

		std::swap(buffer_in, buffer_out);

		for (int component = 0; component < 3; ++component) {
			for (smm y = 0; y < mip_size.y / 2; ++y) {
				thread_pool += [=] () mutable {
					serializer.skip((component * mip_size.y / 2 + y) * 32);
					u64 offset = serializer.current_bit_index();
					u64 skip_to = serializer.read_bits<32>();
					serializer.set_current_bit_index(skip_to);
					for (smm x = 0; x < mip_size.x / 2; ++x) {
						u8 mid = buffer_in[y * padded_dim + x].s[component];
						s8 deltas[2*2];

						if (serializer.read_bits<1>() == 1) {
							if (serializer.read_bits<1>() == 1) {
								if (serializer.read_bits<1>() == 1) {
									// 111
									u32 data = serializer.read_bits<32>();
									deltas[0] = data >>  0;
									deltas[1] = data >>  8;
									deltas[2] = data >> 16;
									deltas[3] = data >> 24;
								} else {
									// 110
									u16 data = serializer.read_bits<16>();
									deltas[0] = (s8)((data << 4) & 0xf0) >> 4;
									deltas[1] = (s8)((data << 0) & 0xf0) >> 4;
									deltas[2] = (s8)((data >> 4) & 0xf0) >> 4;
									deltas[3] = (s8)((data >> 8) & 0xf0) >> 4;
								}
							} else {
								if (serializer.read_bits<1>() == 1) {
									// 101
									u8 data = serializer.read_bits<8>();
									deltas[0] = (s8)((data << 6) & 0xc0) >> 6;
									deltas[1] = (s8)((data << 4) & 0xc0) >> 6;
									deltas[2] = (s8)((data << 2) & 0xc0) >> 6;
									deltas[3] = (s8)((data << 0) & 0xc0) >> 6;
								} else {
									// 100
									u8 data = serializer.read_bits<4>();
									deltas[0] = (s8)((data << 7) & 0x80) >> 7;
									deltas[1] = (s8)((data << 6) & 0x80) >> 7;
									deltas[2] = (s8)((data << 5) & 0x80) >> 7;
									deltas[3] = (s8)((data << 4) & 0x80) >> 7;
								}
							}
						} else {
							// 0
							memset(deltas, 0, sizeof(deltas));
						}
				
						buffer_out[(y*2 + 0) * padded_dim + (x*2 + 0)].s[component] = deltas[0] + mid;
						buffer_out[(y*2 + 0) * padded_dim + (x*2 + 1)].s[component] = deltas[1] + mid;
						buffer_out[(y*2 + 1) * padded_dim + (x*2 + 0)].s[component] = deltas[2] + mid;
						buffer_out[(y*2 + 1) * padded_dim + (x*2 + 1)].s[component] = deltas[3] + mid;
					}
				};
			}
		}
		thread_pool.wait_for_completion(WaitForCompletionOption::do_any_task);
	
		serializer.skip((mip_size.y / 2 * 3) * 32);
		u64 skip_ro = serializer.read_bits<32>();
		serializer.set_current_bit_index(skip_ro);
	}
	
	if (is_p2_square) {
		out.pixels = buffer_out;
	} else {
		out.pixels = DefaultAllocator{}.allocate<Pixel>(unpadded_size.x * unpadded_size.y);
		for (umm y = 0; y < unpadded_size.y; ++y) {
			memcpy(out.pixels + y*unpadded_size.x, buffer_out + y*padded_dim, unpadded_size.x * sizeof(Pixel));
		}
	}

	return out;
}

namespace tim {

Image decode(Span<u8> file) {
	BitSerializer serializer = {};
	serializer.buffer = file.data;
	serializer.cursor = serializer.buffer;

	return decode(serializer);
}

BitSerializer encode(Image image) {
	BitSerializer serializer = {};

	v2u unpadded_size = image.size;
	
	uncompressed_size = unpadded_size.x * unpadded_size.y * sizeof(Pixel);

	if (image.size.x != image.size.y || !is_power_of_2(image.size.x)) {
		// Convert image to power of two square, pad with zeros

		image = pad_to_power_of_2(image);
	}

	serializer.buffer = DefaultAllocator{}.allocate<u8>(image.size.x * image.size.y * 3 * 2);
	serializer.cursor = serializer.buffer;
	
	encode(serializer, image, unpadded_size);


	return serializer;
}

}

void usage() {
	println("Usage: tim [encode(default)|decode] <input> -o <output>");
	println("  encode");
	println("    Convert image to tim");
	println("  decode");
	println("    Convert tim to other format");
}

s32 tl_main(Span<String> args) {
	set_console_encoding(Encoding::utf8);

	//thread_pool.init(0);
	thread_pool.init(get_cpu_info().logical_processor_count - 2);
	defer { thread_pool.deinit(); };

	if (args.count == 1) {
		usage();
		return 1;
	}
	String input_path;
	String output_path;

	bool should_encode = true;
	bool bench = false;

	for (umm i = 0; i < args.count; ++i) {
		if (args[i] == "help") {
			usage();
		} else if (args[i] == "-o") {
			++i;
			if (i >= args.count) {
				println("Expected an output path after -o");
				return 1;
			}
			output_path = args[i];
		} else if (args[i] == "encode") {
			should_encode = true;
		} else if (args[i] == "decode") {
			should_encode = false;
		} else if (args[i] == "bench") {
			bench = true;
		} else {
			input_path = args[i];
		}
	}

	if (!input_path) {
		println("Provide a path to input image");
		return 1;
	}

	if (!output_path) {
		if (should_encode) {
			output_path = format(u8"{}.tim", parse_path(input_path).path_without_extension());
		} else {
			println("Provide a path to output image using -o flag. I need to know the extension.");
			return 1;
		}
	}

	
	if (bench) {
		struct BenchmarkResult {
			char const *name;
			f64 load_secs;
			f64 store_secs;
			umm file_size;
			f64 load_speed() { return uncompressed_size / load_secs; }
			f64 store_speed() { return uncompressed_size / store_secs; }
		};

		StaticList<BenchmarkResult, 6> benchmarks = {};

		void *pixels;
		int x,y;
		auto timer = create_precise_timer();
		const int reps = 9;
		
		uncompressed_size = 4096*4096*3;

		u8 *output_buffer = DefaultAllocator{}.allocate<u8>(128 * MiB);
		u8 *output_cursor = output_buffer;
		
		auto run_benchmark = [&](char const *ext, auto load, auto store, auto free) {
			auto &b = benchmarks.add({ext});
			b.file_size = get_file_size(tformat("tests/test.{}"s, ext)).value();
			auto file = read_entire_file(tformat("tests/test.{}"s, ext));
			println(b.name);
			for (int i = 0; i < reps; ++i) {
				print("{} / {}\r", i + 1, reps);
				reset(timer);
				auto pixels = load(file);
				b.load_secs += reset(timer);
				output_cursor = output_buffer;
				store(pixels);
				b.store_secs += reset(timer);
				free(pixels);
				if (i == 0) {
					b.load_secs = b.store_secs = 0;
				}
			}
			b.load_secs /= reps - 1;
			b.store_secs /= reps - 1;
		};
		
		auto write_func = +[](void *context, void *data, int size) {
			u8 **output_cursor = (u8 **)context;
			memcpy(*output_cursor, data, size);
			*output_cursor += size;
		};
		
		run_benchmark("tim", 
			/*load */[&](Span<u8> file) { return tim::decode(file); },
			/*store*/[&](Image img) { tim::encode(img); },
			/*free */[&](Image &pixels) { /* TODO */ }
		);
		
		run_benchmark("bmp", 
		    /*load */[&](Span<u8> file) { return stbi_load_from_memory(file.data, file.count, &x, &y, 0, 3); },
		    /*store*/[&](stbi_uc *pixels) { return stbi_write_bmp_to_func(write_func, &output_cursor, x, y, 3, pixels); },
		    /*free */[&](void *pixels) { return stbi_image_free(pixels); }
		);
		
		//run_benchmark("png", 
		//    /*load */[&](Span<u8> file) { return stbi_load_from_memory(file.data, file.count, &x, &y, 0, 3); },
		//    /*store*/[&](stbi_uc *pixels) { return stbi_write_png_to_func(write_func, &output_cursor, x, y, 3, pixels, x * 3); },
		//    /*free */[&](void *pixels) { return stbi_image_free(pixels); }
		//);
		//
		//run_benchmark("jpg", 
		//    /*load */[&](Span<u8> file) { return stbi_load_from_memory(file.data, file.count, &x, &y, 0, 3); },
		//    /*store*/[&](stbi_uc *pixels) { return stbi_write_jpg_to_func(write_func, &output_cursor, x, y, 3, pixels, 100); },
		//    /*free */[&](void *pixels) { return stbi_image_free(pixels); }
		//);
		//
		//run_benchmark("tga", 
		//    /*load */[&](Span<u8> file) { return stbi_load_from_memory(file.data, file.count, &x, &y, 0, 3); },
		//    /*store*/[&](stbi_uc *pixels) { return stbi_write_tga_to_func(write_func, &output_cursor, x, y, 3, pixels); },
		//    /*free */[&](void *pixels) { return stbi_image_free(pixels); }
		//);
		//
		//run_benchmark("qoi", 
		//    /*load */[&](Span<u8> file) { return qoi::decode(file).value(); },
		//    /*store*/[&](qoi::Image img) { qoi::encode(img.pixels, img.size); },
		//    /*free */[&](qoi::Image &pixels) { return qoi::free(pixels); }
		//);

		auto ceiled_div = [](umm a, umm b) {
			return (a + b - 1) / b;
		};

		quick_sort(benchmarks.span(), [](BenchmarkResult r) { return r.file_size; });
		println("Format|Size|Percentage|Graph");
		println("-|-|-|-");
		for (auto b : benchmarks) {
			println("{}|{}|{}%|{}",
				b.name,
				FormatFloat{.value = format_bytes(b.file_size), .precision = 1, .trailing_zeros = true},
				b.file_size * 100 / benchmarks.back().file_size,
				Repeat{u8"█", ceiled_div(b.file_size * 20, benchmarks.back().file_size)}
			);
		}
		println();

		quick_sort(benchmarks.span(), [](BenchmarkResult r) { return r.load_secs; });
		println("Format|Dec. speed|Percentage|Graph");
		println("-|-|-|-");
		for (auto b : benchmarks) {
			println("{}|{}/s|{}%|{}",
				b.name,
				FormatFloat{.value = format_bytes(b.load_speed()), .precision = 0},
				(u64)b.load_speed() * 100 / (u64)benchmarks.front().load_speed(),
				Repeat{u8"█", ceiled_div(b.load_speed() * 20, benchmarks.front().load_speed())}
			);
		}
		println();

		quick_sort(benchmarks.span(), [](BenchmarkResult r) { return r.store_secs; });
		println("Format|Enc. speed|Percentage|Graph");
		println("-|-|-|-");
		for (auto b : benchmarks) {
			println("{}|{}/s|{}%|{}",
				b.name,
				FormatFloat{.value = format_bytes(b.store_speed()), .precision = 0},
				(u64)b.store_speed() * 100 / (u64)benchmarks.front().store_speed(),
				Repeat{u8"█", ceiled_div(b.store_speed() * 20, benchmarks.front().store_speed())}
			);
		}
		println();
		return 0;
	}

	if (should_encode) {
		println("encoding");

		Image in;
		if (ends_with(input_path, u8".qoi"s)) {
			auto maybe_image = qoi::decode(read_entire_file(input_path));
			if (!maybe_image) {
				println("Could not decode qoi image");
				return 1;
			}
			auto image = maybe_image.value();
			in.pixels = DefaultAllocator{}.allocate<v3u8>(image.size.x * image.size.y + 64);
			in.size = image.size;

			#if 1
			u8 *src = (u8 *)image.pixels;
			u8 *dst = (u8 *)in.pixels;

			__m128i shuf_indices = _mm_setr_epi8(0,1,2, 4,5,6, 8,9,10, 12,13,14, -1,-1,-1,-1);

			while (src + 64 <= (u8 *)(image.pixels + image.size.x * image.size.y)) {
				auto w1 = _mm_loadu_si128((__m128i *)(src + 0));
				auto w2 = _mm_loadu_si128((__m128i *)(src + 16));
				auto w3 = _mm_loadu_si128((__m128i *)(src + 32));
				auto w4 = _mm_loadu_si128((__m128i *)(src + 48));
				
				// rgbargbargbargba
				// rgbrgbrgbrgb____
				w1 = _mm_shuffle_epi8(w1, shuf_indices);
				w2 = _mm_shuffle_epi8(w2, shuf_indices);
				w3 = _mm_shuffle_epi8(w3, shuf_indices);
				w4 = _mm_shuffle_epi8(w4, shuf_indices);

				_mm_storeu_si128((__m128i *)(dst + 0), w1);
				_mm_storeu_si128((__m128i *)(dst + 12), w2);
				_mm_storeu_si128((__m128i *)(dst + 24), w3);
				_mm_storeu_si128((__m128i *)(dst + 36), w4);

				src += 64;
				dst += 48;
			}
			while (src != (u8 *)(image.pixels + image.size.x * image.size.y)) {
				memcpy(dst, src, 3);
				src += 4;
				dst += 3;
			}
			#else
			for (umm i = 0; i < image.size.x * image.size.y; ++i) {
				in.pixels[i] = image.pixels[i].xyz;
			}
			#endif
		} else {
			in.pixels = autocast stbi_load(autocast null_terminate(input_path).data, autocast &in.size.x, autocast &in.size.y, 0, 3);
		}

		auto serializer = tim::encode(in);
		
		write_entire_file(output_path, serializer.span());


		auto n_bits_sum = n_bits0 + n_bits1 + n_bits2 + n_bits4 + n_bits8;

		println("Compressed size: {} ({}% of uncompressed)", format_bytes(serializer.span().count), serializer.span().count * 100.0f / uncompressed_size);
		println("    n_bits0: {} ({}%)", n_bits0, n_bits0 * 100.0f / n_bits_sum);
		println("    n_bits1: {} ({}%)", n_bits1, n_bits1 * 100.0f / n_bits_sum);
		println("    n_bits2: {} ({}%)", n_bits2, n_bits2 * 100.0f / n_bits_sum);
		println("    n_bits4: {} ({}%)", n_bits4, n_bits4 * 100.0f / n_bits_sum);
		println("    n_bits8: {} ({}%)", n_bits8, n_bits8 * 100.0f / n_bits_sum);
	} else {
		println("decoding");

		BitSerializer serializer = {};
		serializer.buffer = read_entire_file(input_path, {.extra_space_after = 64}).data;
		serializer.cursor = serializer.buffer;


		Image out = decode(serializer);
		
		if (ends_with(output_path, u8".bmp"s)) {
			stbi_write_bmp(autocast null_terminate(output_path).data, out.size.x, out.size.y, 3, out.pixels);
		} else if (ends_with(output_path, u8".png"s)) {
			stbi_write_png(autocast null_terminate(output_path).data, out.size.x, out.size.y, 3, out.pixels, out.size.x * sizeof(Pixel));
		} else if (ends_with(output_path, u8".jpg"s)) {
			stbi_write_jpg(autocast null_terminate(output_path).data, out.size.x, out.size.y, 3, out.pixels, 100);
		} else if (ends_with(output_path, u8".tga"s)) {
			stbi_write_tga(autocast null_terminate(output_path).data, out.size.x, out.size.y, 3, out.pixels);
		} else if (ends_with(output_path, u8".qoi"s)) {
			write_entire_file(output_path, qoi::encode(out.pixels, out.size));
		} else {
			println("Unknown output extension");
			return 1;
		}
	}

	return 0;
}
