#include <cstdint>

typedef uint64_t RNG_t;

class RNG {
public:
	RNG();
	
	void init();
	void reset();
	uint64_t getSeed();
	void setSeed(uint64_t);
	
	RNG_t next();
	RNG_t* createArray(size_t);
	void fillArray(RNG_t*, size_t);
	
	static const char* getName();
	
protected:
	RNG_t seed;
	
#ifndef RNG_CHACHA_ROUNDS
	RNG_t val;
#else
	void chachaBlock();
	uint32_t state[16];
	uint32_t keystream[16];
	uint_fast8_t keystreamIdx=100;
#endif
};
