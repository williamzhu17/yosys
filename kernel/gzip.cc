#include "kernel/yosys_common.h"
#include "kernel/log.h"
#include "kernel/gzip.h"
#include <iostream>
#include <dirent.h>
#include <string>
#include <cstdarg>
#include <cstdio>

YOSYS_NAMESPACE_BEGIN


#ifdef YOSYS_ENABLE_ZLIB

PRIVATE_NAMESPACE_BEGIN

using namespace Zlib;

PRIVATE_NAMESPACE_END

gzip_ostream::obuf::obuf() {
    setp(buffer, buffer + buffer_size - 1);
}

bool gzip_ostream::obuf::open(const std::string &filename) {
    gzf = gzopen(filename.c_str(), "wb");
    return gzf != nullptr;
}

int gzip_ostream::obuf::sync() {
    int num = pptr() - pbase();
    if (num > 0) {
        if (gzwrite(gzf, reinterpret_cast<const void*>(pbase()), num) != num) {
            return -1;
        }
        pbump(-num);
    }
    return 0;
}

gzip_ostream::obuf::~obuf() {
    if (gzf) {
        sync();
        gzclose(gzf);
    }
}

bool gzip_istream::ibuf::open(const std::string& filename) {
	if (gzf) {
		Zlib::gzclose(gzf);
	}
	gzf = Zlib::gzopen(filename.c_str(), "rb");
	if (!gzf) {
		return false;
	}
	// Empty and point to start
	setg(buffer, buffer, buffer);
	return true;
}

// Called when the buffer is empty and more input is needed
std::istream::int_type gzip_istream::ibuf::underflow() {
	log_assert(gzf && "No gzfile opened\n");
	int bytes_read = Zlib::gzread(gzf, buffer, buffer_size);
	if (bytes_read <= 0) {
		if (Zlib::gzeof(gzf))
			return traits_type::eof();

		int err;
		const char* error_msg = Zlib::gzerror(gzf, &err);
		if (err != Z_OK)
			log_error("%s", error_msg);
		else
			log_error("Decompression logic failure: "\
					"read <=0 bytes but neither EOF nor error\n");
	}

	// Keep size and point to start
	setg(buffer, buffer, buffer + bytes_read);
	return traits_type::to_int_type(buffer[0]);
}

gzip_istream::ibuf::~ibuf() {
	if (gzf) {
		int err = Zlib::gzclose(gzf);
		if (err != Z_OK) {
			// OK to overwrite rr it, it doesn't change
			const char* error_msg = Zlib::gzerror(gzf, &err);
			log_error("%s", error_msg);
		}
	}
}

#endif // YOSYS_ENABLE_ZLIB


// Takes a successfully opened ifstream. If it's gzipped, returns an istream. Otherwise,
// returns the original ifstream, rewound to the start.
std::istream* uncompressed(std::ifstream* f, const std::string filename) {
	if (!f)
		return nullptr;
	// Check for gzip magic
	unsigned char magic[3];
	int n = 0;
	while (n < 3)
	{
		int c = f->get();
		if (c != EOF) {
			magic[n] = (unsigned char) c;
		}
		n++;
	}
	if (n == 3 && magic[0] == 0x1f && magic[1] == 0x8b) {
#ifdef YOSYS_ENABLE_ZLIB
		log("Found gzip magic in file `%s', decompressing using zlib.\n", filename.c_str());
		if (magic[2] != 8)
			log_cmd_error("gzip file `%s' uses unsupported compression type %02x\n",
				filename.c_str(), unsigned(magic[2]));
		delete f;
		gzip_istream* s = new gzip_istream();
		return s->open(filename.c_str()) ? s : nullptr;
#else
		log_cmd_error("File `%s' is a gzip file, but Yosys is compiled without zlib.\n", filename.c_str());
#endif // YOSYS_ENABLE_ZLIB
	} else {
		f->clear();
		f->seekg(0, std::ios::beg);
		return f;
	}
}

YOSYS_NAMESPACE_END
