// 把「记录喂奶」暴露给小智官方服务器上的大模型
//
// 官方服务器只负责 ASR / LLM / TTS，「记账」这个动作由板子自己完成：
//   LLM 判定要调工具 -> tools/call 走已有 WebSocket 下来 -> 这里执行 -> 返回结果给它念
// 所以这套东西不需要公网服务器、不需要 NAS，只要板子能上网。
#pragma once

namespace baby_tools {

// 在板型初始化时调用一次（幂等）
void Register();

}  // namespace baby_tools
