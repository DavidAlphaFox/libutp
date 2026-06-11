#pragma once

// 应用层消息分帧
//
// uTP 与 TCP 一样是字节流：UTP_ON_READ 给出的数据边界与消息边界无关，
// 应用必须自己分帧。本协议：
//
//   [4B 小端 长度 n][1B 类型][n-1 字节正文]
//
// 长度字段计入类型字节，不计入自身。

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace chat {

enum class MsgType : uint8_t {
	join = 1,  // 正文 = 昵称（客户端入场第一帧）
	text = 2,  // 正文 = 聊天内容（服务端广播时已带 "昵称: " 前缀）
	sys  = 3,  // 正文 = 系统通知（join/leave 等）
};

// 单帧长度上限：防御恶意/损坏的长度字段导致重组缓冲无限增长
inline constexpr uint32_t kMaxFrame = 64 * 1024;

inline std::vector<uint8_t> encode(MsgType type, std::string_view body) {
	const uint32_t n = 1 + (uint32_t)body.size();
	std::vector<uint8_t> f(4 + n);
	f[0] = (uint8_t)(n);
	f[1] = (uint8_t)(n >> 8);
	f[2] = (uint8_t)(n >> 16);
	f[3] = (uint8_t)(n >> 24);
	f[4] = (uint8_t)type;
	std::memcpy(f.data() + 5, body.data(), body.size());
	return f;
}

struct Frame {
	MsgType type;
	std::vector<uint8_t> body;

	std::string_view text() const {
		return { (const char*)body.data(), body.size() };
	}
};

// 流式分帧器：feed() 喂入任意切片，next() 取出完整帧
class FrameParser {
public:
	void feed(std::span<const uint8_t> d) {
		buf_.insert(buf_.end(), d.begin(), d.end());
	}

	// 返回 nullopt 表示数据不足；坏帧（超长）时丢弃缓冲并置 broken()
	std::optional<Frame> next() {
		if (broken_ || buf_.size() < 4) return std::nullopt;
		const uint32_t n = (uint32_t)buf_[0] | (uint32_t)buf_[1] << 8 |
		                   (uint32_t)buf_[2] << 16 | (uint32_t)buf_[3] << 24;
		if (n < 1 || n > kMaxFrame) {
			broken_ = true;
			buf_.clear();
			return std::nullopt;
		}
		if (buf_.size() < 4 + (size_t)n) return std::nullopt;

		Frame fr{ .type = (MsgType)buf_[4],
		          .body = { buf_.begin() + 5, buf_.begin() + 4 + n } };
		buf_.erase(buf_.begin(), buf_.begin() + 4 + n);
		return fr;
	}

	bool broken() const { return broken_; }

private:
	std::vector<uint8_t> buf_;
	bool broken_ = false;
};

}  // namespace chat
