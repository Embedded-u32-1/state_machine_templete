#include "basic_fsm/basic_fsm.h"
#include <iostream>
#include <map>

// ==================== 简单状态定义 ====================
enum class LightState { kOff, kOn, kBlinking };

// ==================== 简单测试类 ====================
class Light {
public:
    using Fsm = BasicFsm<LightState>;
    using StateActions = Fsm::StateActions;
    using Resolver = Fsm::Resolver;
    using Validator = Fsm::Validator;

    Light()
        : fsm_(LightState::kOff,
               BuildStateActions(),
               [this](LightState current) { return ResolveNext(current); },
               nullptr) {}

    // 模拟外部触发：开关
    void Toggle() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_action_ = "toggle";
        fsm_.Sync();
    }

    // 模拟外部触发：闪烁
    void Blink() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_action_ = "blink";
        fsm_.Sync();
    }

    LightState CurrentState() const { return fsm_.CurrentState(); }

    const char* StateToString(LightState s) const {
        switch (s) {
            case LightState::kOff: return "Off";
            case LightState::kOn: return "On";
            case LightState::kBlinking: return "Blinking";
            default: return "Unknown";
        }
    }

private:
    static std::map<LightState, StateActions> BuildStateActions() {
        return {
            {LightState::kOff, {
                [] (auto& fsm) { std::cout << "[Enter] 灯已关闭\n"; },
                [] (auto& fsm) { std::cout << "[Exit] 灯关闭中...\n"; },
                [] (auto& fsm) { std::cout << "[Hold] 灯保持关闭状态\n"; }
            }},
            {LightState::kOn, {
                [] (auto& fsm) { std::cout << "[Enter] 灯已打开\n"; },
                [] (auto& fsm) { std::cout << "[Exit] 灯关闭中...\n"; },
                [] (auto& fsm) { std::cout << "[Hold] 灯保持打开状态\n"; }
            }},
            {LightState::kBlinking, {
                [] (auto& fsm) { std::cout << "[Enter] 灯开始闪烁\n"; },
                [] (auto& fsm) { std::cout << "[Exit] 停止闪烁\n"; },
                [] (auto& fsm) { std::cout << "[Hold] 灯持续闪烁\n"; }
            }}
        };
    }

    LightState ResolveNext(LightState current) {
        if (pending_action_.empty())
            return current;

        if (current == LightState::kOff && pending_action_ == "toggle")
            return LightState::kOn;
        if (current == LightState::kOn && pending_action_ == "toggle")
            return LightState::kOff;
        if (current == LightState::kOn && pending_action_ == "blink")
            return LightState::kBlinking;
        if (current == LightState::kBlinking && pending_action_ == "toggle")
            return LightState::kOff;
        
        return current;
    }

private:
    Fsm fsm_;
    std::string pending_action_;
    std::mutex mutex_;
};

// ==================== 主函数 ====================
int main() {
    std::cout << "===== BasicFsm 简单测试 =====\n\n";

    Light light;

    std::cout << "初始状态: " << light.StateToString(light.CurrentState()) << "\n\n";

    std::cout << "--- 执行：Toggle (Off -> On) ---\n";
    light.Toggle();
    std::cout << "当前状态: " << light.StateToString(light.CurrentState()) << "\n\n";

    std::cout << "--- 执行：Toggle (On -> Off) ---\n";
    light.Toggle();
    std::cout << "当前状态: " << light.StateToString(light.CurrentState()) << "\n\n";

    std::cout << "--- 执行：Toggle (Off -> On) ---\n";
    light.Toggle();
    std::cout << "当前状态: " << light.StateToString(light.CurrentState()) << "\n\n";

    std::cout << "--- 执行：Blink (On -> Blinking) ---\n";
    light.Blink();
    std::cout << "当前状态: " << light.StateToString(light.CurrentState()) << "\n\n";

    std::cout << "--- 执行：Toggle (Blinking -> Off) ---\n";
    light.Toggle();
    std::cout << "当前状态: " << light.StateToString(light.CurrentState()) << "\n\n";

    std::cout << "===== 测试完成 =====\n";
    return 0;
}