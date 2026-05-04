#include "../event_fsm.h"
#include <iostream>
#include <map>
#ifdef _WIN32
#include <windows.h>
#endif

// ==================== 状态和事件定义 ====================
enum class PlayerState { kStopped, kPlaying, kPaused };

enum class PlayerEvent { kPlay, kPause, kStop };

namespace {

void ConfigureConsoleUtf8() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

}  // namespace

// ==================== 测试类：播放器 ====================
class Player {
public:
    using Fsm = EventFsm<PlayerState, PlayerEvent>;
    using Action = Fsm::Action;
    using Key = Fsm::Key;
    using Lifecycle = Fsm::Lifecycle;
    using Resolver = Fsm::Resolver;
    using Validator = Fsm::Validator;
    using TransitionHook = Fsm::TransitionHook;

    Player()
        : fsm_(PlayerState::kStopped,
               BuildRules(),
               BuildLifecycles(),
               [this](PlayerState current, const std::vector<PlayerEvent>& events) {
                   return ResolveNext(current, events);
               },
               nullptr,
               [this](PlayerState from, PlayerState to) {
                   OnTransition(from, to);
               },
               10) {}

    // 投递事件
    void PostEvent(PlayerEvent event) {
        fsm_.Post(event);
    }

    // 同步处理事件
    void Sync() {
        fsm_.Sync();
    }

    // 强制设置状态
    void SetState(PlayerState state) {
        fsm_.SetState(state);
    }

    PlayerState CurrentState() const { return fsm_.CurrentState(); }

    const char* StateToString(PlayerState s) const {
        switch (s) {
            case PlayerState::kStopped: return "Stopped";
            case PlayerState::kPlaying: return "Playing";
            case PlayerState::kPaused: return "Paused";
            default: return "Unknown";
        }
    }

    const char* EventToString(PlayerEvent e) const {
        switch (e) {
            case PlayerEvent::kPlay: return "Play";
            case PlayerEvent::kPause: return "Pause";
            case PlayerEvent::kStop: return "Stop";
            default: return "Unknown";
        }
    }

private:
    // 构建事件规则表
    static std::map<Key, Action> BuildRules() {
        return {
            // Stopped 状态下的规则
            {Key{PlayerState::kStopped, PlayerEvent::kPlay},
             [](auto& fsm) { std::cout << "[Action] 开始播放\n"; }},
            {Key{PlayerState::kStopped, PlayerEvent::kStop},
             [](auto& fsm) { std::cout << "[Action] 已经停止，无需操作\n"; }},

            // Playing 状态下的规则
            {Key{PlayerState::kPlaying, PlayerEvent::kPause},
             [](auto& fsm) { std::cout << "[Action] 暂停播放\n"; }},
            {Key{PlayerState::kPlaying, PlayerEvent::kStop},
             [](auto& fsm) { std::cout << "[Action] 停止播放\n"; }},
            {Key{PlayerState::kPlaying, PlayerEvent::kPlay},
             [](auto& fsm) { std::cout << "[Action] 正在播放中\n"; }},

            // Paused 状态下的规则
            {Key{PlayerState::kPaused, PlayerEvent::kPlay},
             [](auto& fsm) { std::cout << "[Action] 继续播放\n"; }},
            {Key{PlayerState::kPaused, PlayerEvent::kStop},
             [](auto& fsm) { std::cout << "[Action] 停止播放\n"; }},
            {Key{PlayerState::kPaused, PlayerEvent::kPause},
             [](auto& fsm) { std::cout << "[Action] 已经暂停\n"; }}
        };
    }

    // 构建状态生命周期表
    static std::map<PlayerState, Lifecycle> BuildLifecycles() {
        return {
            {PlayerState::kStopped, {
                [] (auto& fsm) { std::cout << "[Enter] 进入停止状态\n"; },
                [] (auto& fsm) { std::cout << "[Exit] 离开停止状态\n"; }
            }},
            {PlayerState::kPlaying, {
                [] (auto& fsm) { std::cout << "[Enter] 进入播放状态\n"; },
                [] (auto& fsm) { std::cout << "[Exit] 离开播放状态\n"; }
            }},
            {PlayerState::kPaused, {
                [] (auto& fsm) { std::cout << "[Enter] 进入暂停状态\n"; },
                [] (auto& fsm) { std::cout << "[Exit] 离开暂停状态\n"; }
            }}
        };
    }

    // 仲裁器：根据当前状态和待处理事件列表决定目标状态
    PlayerState ResolveNext(PlayerState current, const std::vector<PlayerEvent>& events) {
        if (events.empty())
            return current;

        // 仲裁逻辑：根据当前状态和事件列表决定目标状态
        // 注意：这里简化处理，只取最后一个事件作为决策依据
        // 实际应用中可以根据业务需求实现更复杂的仲裁逻辑
        PlayerEvent last_event = events.back();

        switch (current) {
            case PlayerState::kStopped:
                if (last_event == PlayerEvent::kPlay)
                    return PlayerState::kPlaying;
                return PlayerState::kStopped;

            case PlayerState::kPlaying:
                if (last_event == PlayerEvent::kPause)
                    return PlayerState::kPaused;
                if (last_event == PlayerEvent::kStop)
                    return PlayerState::kStopped;
                return PlayerState::kPlaying;

            case PlayerState::kPaused:
                if (last_event == PlayerEvent::kPlay)
                    return PlayerState::kPlaying;
                if (last_event == PlayerEvent::kStop)
                    return PlayerState::kStopped;
                return PlayerState::kPaused;

            default:
                return current;
        }
    }

    // 状态转移钩子
    void OnTransition(PlayerState from, PlayerState to) {
        std::cout << "[Transition] " << StateToString(from) << " -> " << StateToString(to) << "\n";
    }

private:
    Fsm fsm_;
};

// ==================== 主函数 ====================
int main() {
    ConfigureConsoleUtf8();
    std::cout << "===== EventFsm 简单测试 =====\n\n";

    Player player;

    std::cout << "初始状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试1: Stopped -> Playing (Play)
    std::cout << "--- 测试1: Stopped + Play -> Playing ---\n";
    player.PostEvent(PlayerEvent::kPlay);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试2: Playing -> Paused (Pause)
    std::cout << "--- 测试2: Playing + Pause -> Paused ---\n";
    player.PostEvent(PlayerEvent::kPause);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试3: Paused -> Playing (Play)
    std::cout << "--- 测试3: Paused + Play -> Playing ---\n";
    player.PostEvent(PlayerEvent::kPlay);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试4: Playing -> Stopped (Stop)
    std::cout << "--- 测试4: Playing + Stop -> Stopped ---\n";
    player.PostEvent(PlayerEvent::kStop);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试5: Stopped + Stop -> Stopped (状态保持)
    std::cout << "--- 测试5: Stopped + Stop -> Stopped (状态保持) ---\n";
    player.PostEvent(PlayerEvent::kStop);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试6: 多个事件批量投递
    std::cout << "--- 测试6: 批量投递事件 (Play -> Pause -> Stop) ---\n";
    player.PostEvent(PlayerEvent::kPlay);
    player.PostEvent(PlayerEvent::kPause);
    player.PostEvent(PlayerEvent::kStop);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试7: 强制设置状态
    std::cout << "--- 测试7: 强制设置状态为 Playing ---\n";
    player.SetState(PlayerState::kPlaying);
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    // 测试8: Playing + Play -> Playing (状态保持)
    std::cout << "--- 测试8: Playing + Play -> Playing (状态保持) ---\n";
    player.PostEvent(PlayerEvent::kPlay);
    player.Sync();
    std::cout << "当前状态: " << player.StateToString(player.CurrentState()) << "\n\n";

    std::cout << "===== 测试完成 =====\n";
    return 0;
}
