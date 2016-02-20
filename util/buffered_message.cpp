#include "buffered_message.h"

raw_message::raw_message() : read_length(0), write_length(0) { }

raw_message::raw_message(raw_message const &other) :
        read_length(other.read_length), write_length(other.write_length) {
    for (size_t i = 0; i < other.read_length; i++) {
        buffer[i] = other.buffer[i];
    }
}

raw_message::raw_message(raw_message &&other) {
    swap(*this, other);
}

raw_message &raw_message::operator=(raw_message other) {
    swap(*this, other);
    return *this;
}

bool raw_message::can_read() const {
    return read_length < BUFFER_LENGTH;
}

bool raw_message::can_write() const {
    return write_length < read_length;
}

void raw_message::read_from(file_descriptor const &fd) {
    read_length += fd.read(buffer + read_length, BUFFER_LENGTH - read_length);
}

void raw_message::write_to(file_descriptor const &fd) {
    write_length += fd.write(buffer + write_length, read_length - write_length);
    if (write_length == BUFFER_LENGTH) {
        read_length = 0;
        write_length = 0;
    }
}

void swap(raw_message& first, raw_message& second) {
    using std::swap;
    swap(first.read_length, second.read_length);
    swap(first.write_length, second.write_length);

    for (size_t i = 0; i < (first.read_length > second.read_length) ? first.read_length : second.read_length; i++) {
        swap(first.buffer[i], second.buffer[i]);
    }
}

