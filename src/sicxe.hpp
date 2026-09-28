#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

class SicXe {
public:
    static constexpr uint32_t memory_size = 1u << 20;
    static constexpr uint32_t word_mask = 0xffffff;
    enum Register { A=0, X=1, L=2, B=3, S=4, T=5, F=6, PC=8, SW=9 };

    SicXe();
    void load(const std::string& object, uint32_t address = 0);
    void reset();
    void step();
    size_t run(size_t limit = 10000, int32_t breakpoint = -1);
    void add_input(const std::string& data);
    uint8_t byte(uint32_t address) const;
    uint32_t word(uint32_t address) const;
    uint32_t reg(Register r) const { return registers_[r]; }
    uint64_t floating() const { return floating_; }
    int condition() const { return condition_; }
    bool halted() const { return halted_; }
    const std::string& error() const { return error_; }
    const std::string& output() const { return output_; }
    const std::string& name() const { return name_; }
    uint32_t start() const { return start_; }
    uint32_t length() const { return length_; }
    uint64_t steps() const { return steps_; }
    size_t pending_input() const { return input_.size(); }

private:
    std::vector<uint8_t> memory_;
    std::array<uint32_t, 10> registers_{};
    uint64_t floating_ = 0;
    int condition_ = 0;
    bool halted_ = true;
    std::string error_, output_, name_, object_;
    std::deque<uint8_t> input_;
    uint32_t start_ = 0, length_ = 0, entry_ = 0, load_address_ = 0;
    uint64_t steps_ = 0;

    void put_byte(uint32_t address, uint8_t value);
    void put_word(uint32_t address, uint32_t value);
    uint64_t read48(uint32_t address) const;
    void write48(uint32_t address, uint64_t value);
    static int32_t signed24(uint32_t value);
    static uint32_t mask24(int64_t value);
    void compare(int64_t left, int64_t right);
    double decode_float(uint64_t bits) const;
    uint64_t encode_float(double value) const;
};
