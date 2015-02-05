#ifndef OSMIUM_IO_COMPRESSION_HPP
#define OSMIUM_IO_COMPRESSION_HPP

/*

This file is part of Osmium (http://osmcode.org/libosmium).

Copyright 2013-2015 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <cerrno>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifndef _MSC_VER
# include <unistd.h>
#else
# include <io.h>
#endif

#include <osmium/io/detail/read_write.hpp>
#include <osmium/io/file_compression.hpp>
#include <osmium/util/compatibility.hpp>

namespace osmium {

    namespace io {

        class Compressor {

        public:

            Compressor() = default;

            virtual ~Compressor() {
            }

            virtual void write(const std::string& data) = 0;

            virtual void close() = 0;

        }; // class Compressor

        class Decompressor {

        public:

            static constexpr size_t input_buffer_size = 256 * 1024;

            Decompressor() = default;

            Decompressor(const Decompressor&) = delete;
            Decompressor& operator=(const Decompressor&) = delete;

            Decompressor(Decompressor&&) = delete;
            Decompressor& operator=(Decompressor&&) = delete;

            virtual ~Decompressor() {
            }

            virtual std::string read() = 0;

            virtual void close() {
            }

        }; // class Decompressor

        /**
         * This singleton factory class is used to register compression
         * algorithms used for reading and writing OSM files.
         *
         * For each algorithm we store two functions that construct
         * a compressor and decompressor object, respectively.
         */
        class CompressionFactory {

        public:

            typedef std::function<osmium::io::Compressor*(int)> create_compressor_type;
            typedef std::function<osmium::io::Decompressor*(int)> create_decompressor_type_fd;
            typedef std::function<osmium::io::Decompressor*(const char*, size_t)> create_decompressor_type_buffer;

        private:

            typedef std::map<const osmium::io::file_compression, std::tuple<create_compressor_type, create_decompressor_type_fd, create_decompressor_type_buffer>> compression_map_type;

            compression_map_type m_callbacks;

            CompressionFactory() = default;

            CompressionFactory(const CompressionFactory&) = delete;
            CompressionFactory& operator=(const CompressionFactory&) = delete;

            CompressionFactory(CompressionFactory&&) = delete;
            CompressionFactory& operator=(CompressionFactory&&) = delete;

            OSMIUM_NORETURN void error(osmium::io::file_compression compression) {
                std::string error_message {"Support for compression '"};
                error_message += as_string(compression);
                error_message += "' not compiled into this binary.";
                throw std::runtime_error(error_message);
            }

        public:

            static CompressionFactory& instance() {
                static CompressionFactory factory;
                return factory;
            }

            bool register_compression(
                osmium::io::file_compression compression,
                create_compressor_type create_compressor,
                create_decompressor_type_fd create_decompressor_fd,
                create_decompressor_type_buffer create_decompressor_buffer) {

                compression_map_type::value_type cc(compression, std::make_tuple(create_compressor, create_decompressor_fd, create_decompressor_buffer));
                return m_callbacks.insert(cc).second;
            }

            std::unique_ptr<osmium::io::Compressor> create_compressor(osmium::io::file_compression compression, int fd) {
                auto it = m_callbacks.find(compression);

                if (it != m_callbacks.end()) {
                    return std::unique_ptr<osmium::io::Compressor>(std::get<0>(it->second)(fd));
                }

                error(compression);
            }

            std::unique_ptr<osmium::io::Decompressor> create_decompressor(osmium::io::file_compression compression, int fd) {
                auto it = m_callbacks.find(compression);

                if (it != m_callbacks.end()) {
                    return std::unique_ptr<osmium::io::Decompressor>(std::get<1>(it->second)(fd));
                }

                error(compression);
            }

            std::unique_ptr<osmium::io::Decompressor> create_decompressor(osmium::io::file_compression compression, const char* buffer, size_t size) {
                auto it = m_callbacks.find(compression);

                if (it != m_callbacks.end()) {
                    return std::unique_ptr<osmium::io::Decompressor>(std::get<2>(it->second)(buffer, size));
                }

                error(compression);
            }

        }; // class CompressionFactory

        class NoCompressor : public Compressor {

            int m_fd;

        public:

            NoCompressor(int fd) :
                Compressor(),
                m_fd(fd) {
            }

            ~NoCompressor() override final {
                close();
            }

            void write(const std::string& data) override final {
                osmium::io::detail::reliable_write(m_fd, data.data(), data.size());
            }

            void close() override final {
                if (m_fd >= 0) {
                    ::close(m_fd);
                    m_fd = -1;
                }
            }

        }; // class NoCompressor

        class NoDecompressor : public Decompressor {

            int m_fd;
            const char *m_buffer;
            size_t m_buffer_size;

        public:

            NoDecompressor(int fd) :
                Decompressor(),
                m_fd(fd),
                m_buffer(nullptr),
                m_buffer_size(0) {
            }

            NoDecompressor(const char* buffer, size_t size) :
                Decompressor(),
                m_fd(-1),
                m_buffer(buffer),
                m_buffer_size(size) {
            }

            ~NoDecompressor() override final {
                close();
            }

            std::string read() override final {
                std::string buffer;

                if (m_buffer) {
                    if (m_buffer_size != 0) {
                        size_t size = m_buffer_size;
                        m_buffer_size = 0;
                        buffer.append(m_buffer, size);
                    }
                } else {
                    buffer.resize(osmium::io::Decompressor::input_buffer_size);
                    ssize_t nread = ::read(m_fd, const_cast<char*>(buffer.data()), buffer.size());
                    if (nread < 0) {
                        throw std::system_error(errno, std::system_category(), "Read failed");
                    }
                    buffer.resize(static_cast<size_t>(nread));
                }

                return buffer;
            }

            void close() override final {
                if (m_fd >= 0) {
                    ::close(m_fd);
                    m_fd = -1;
                }
            }

        }; // class NoDecompressor

        namespace {

            const bool registered_no_compression = osmium::io::CompressionFactory::instance().register_compression(osmium::io::file_compression::none,
                [](int fd) { return new osmium::io::NoCompressor(fd); },
                [](int fd) { return new osmium::io::NoDecompressor(fd); },
                [](const char* buffer, size_t size) { return new osmium::io::NoDecompressor(buffer, size); }
            );

        } // anonymous namespace

    } // namespace io

} // namespace osmium

#endif // OSMIUM_IO_COMPRESSION_HPP
