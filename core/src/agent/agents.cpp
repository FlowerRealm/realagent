#include "agent/agents.hpp"

namespace realagent {

/* 拆一个 agent 分两步，中间必须放开图锁。
 *
 * ~Agent 会 join 它那条线程，而那条线程跑完时要拿图锁去投完成通知——
 * 持着锁 join 就是等一个在等我的人。所以：先在锁里把它从表里摘出来（此后
 * on_done 扫不到它，投递自然成了空操作），放开锁，再让它析构。 */

namespace {

/* 创建方能不能把 who 写进新 agent 的边里：只能是它自己，或它已有出边的那些。
 * by 为空是人建的——人不是图上的节点，所以一条边都不许给。 */
bool may_name(const std::unordered_map<std::string, std::unordered_set<std::string>> &edges,
              const std::string &by, const std::string &who)
{
    if (by.empty()) return false;
    if (who == by) return true;
    const auto it = edges.find(by);
    return it != edges.end() && it->second.count(who) > 0;
}

} // namespace

Agents::~Agents()
{
    std::map<std::string, std::unique_ptr<Agent>> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        doomed.swap(agents_); // 表先空掉
    }
    doomed.clear(); // 这里才 join，图锁没被我们持着
}

std::string Agents::create(const std::string &client, const std::string &workdir,
                           const std::string &by, const std::vector<std::string> &in,
                           const std::vector<std::string> &out, std::string &err)
{
    err.clear(); // 出参先清：不清的话上一次失败留下的话会挂在这次成功上
    if (workdir.empty())
    {
        err = "workdir 必传，没有默认值";
        return {};
    }
    std::lock_guard<std::mutex> lk(mtx_);

    // 组随创建者，不可能属于别的组（ADR-0021）。
    // 按 find 查而不是 operator[]：后者在键不存在时会插一条空的进去，
    // 于是一次失败的创建会在表里留下一个谁也不是的组
    const auto by_owner = owner_.find(by);
    if (!by.empty() && by_owner == owner_.end())
    {
        err = "无此 agent: " + by;
        return {};
    }
    const std::string group = by.empty() ? client : by_owner->second;
    if (group.empty())
    {
        err = "缺 client_id";
        return {};
    }

    for (const auto *list : {&in, &out})
        for (const std::string &who : *list)
        {
            const auto o = owner_.find(who);
            if (!agents_.count(who) || o == owner_.end() || o->second != group)
            {
                err = "无此 agent: " + who;
                return {};
            }
            if (!may_name(edges_, by, who))
            {
                err = "不能把 " + who + " 写进边里——你自己都不认识它";
                return {};
            }
        }

    const std::string id = "a" + std::to_string(++next_id_);
    owner_[id] = group;
    // 派生出来的（by 非空）会话落 sessions/sub/，不进会话清单（ADR-0021）。
    // 留不留记录由「谁创建的」决定，不是一个参数——那个决定不该由模型做
    agents_[id] = std::make_unique<Agent>(ctx_, approval_, workdir, id, this, !by.empty());
    for (const std::string &x : in) edges_[x].insert(id);
    for (const std::string &x : out) edges_[id].insert(x);
    return id;
}

bool Agents::has_edge(const std::string &from, const std::string &to) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = edges_.find(from);
    return it != edges_.end() && it->second.count(to) > 0;
}

bool Agents::post(const std::string &to, const std::string &from, const std::string &text)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = agents_.find(to);
    if (it == agents_.end()) return false;

    // 收方有指向发方的边才看得见发信人；没有就跟人发的一模一样
    const auto e = edges_.find(to);
    const bool known = !from.empty() && e != edges_.end() && e->second.count(from) > 0;
    it->second->post(known ? "[来自 " + from + "] " + text : text);
    return true;
}

void Agents::on_done(const std::string &id, const std::string &summary)
{
    std::lock_guard<std::mutex> lk(mtx_);
    // 扫入边：谁指向我，谁就关心我跑完了没有
    for (const auto &[from, outs] : edges_)
    {
        if (!outs.count(id)) continue;
        const auto it = agents_.find(from);
        if (it != agents_.end()) it->second->post("[" + id + " 已完成] " + summary);
    }
}

void Agents::interrupt(const std::string &id)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = agents_.find(id);
    if (it != agents_.end()) it->second->interrupt();
}

void Agents::close(const std::string &id)
{
    std::unique_ptr<Agent> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto it = agents_.find(id);
        if (it == agents_.end()) return;
        it->second->interrupt(); // 先停下来，别等它自己跑完
        doomed = std::move(it->second);
        agents_.erase(it);
        owner_.erase(id);
        // 指向它的边一起删：边的语义是「我知道它存在」，它不存在了这条边就是假的。
        // 删边不是清理动作，是让边继续说真话
        edges_.erase(id);
        for (auto &[_, outs] : edges_) outs.erase(id);
    }
    doomed.reset(); // 出了锁才 join
}

nlohmann::json Agents::list(const std::string &client) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &[id, a] : agents_)
    {
        if (owner_.at(id) != client) continue; // 只列自己那一组
        nlohmann::json e{{"id", id},
                         {"workdir", a->workdir()},
                         {"state", a->running() ? "running" : "idle"},
                         {"session_id", a->session_id()}};
        nlohmann::json outs = nlohmann::json::array(), ins = nlohmann::json::array();
        if (const auto it = edges_.find(id); it != edges_.end())
            for (const std::string &to : it->second) outs.push_back(to);
        for (const auto &[from, set] : edges_)
            if (set.count(id)) ins.push_back(from);
        e["out_edges"] = std::move(outs);
        e["in_edges"] = std::move(ins);
        arr.push_back(std::move(e));
    }
    return arr;
}

std::map<std::string, std::string> Agents::openers(const std::string &client) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::map<std::string, std::string> out;
    for (const auto &[id, a] : agents_)
        if (owner_.at(id) == client) out[a->session_id()] = id;
    return out;
}

Agent *Agents::locked_find(const std::string &client, const std::string &id) const
{
    const auto it = agents_.find(id);
    if (it == agents_.end()) return nullptr;
    // 跨组一律当不存在：区分「不存在」与「不是你的」等于告诉调用方别的组里有什么
    const auto o = owner_.find(id);
    if (o == owner_.end() || o->second != client) return nullptr;
    return it->second.get();
}

Agent *Agents::find(const std::string &client, const std::string &id)
{
    std::lock_guard<std::mutex> lk(mtx_);
    return locked_find(client, id);
}

void Agents::close_group(const std::string &client)
{
    std::vector<std::unique_ptr<Agent>> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = agents_.begin(); it != agents_.end();)
        {
            const auto o = owner_.find(it->first);
            if (o == owner_.end() || o->second != client)
            {
                ++it;
                continue;
            }
            it->second->interrupt(); // 先全停下来，再逐个拆
            const std::string id = it->first;
            doomed.push_back(std::move(it->second));
            it = agents_.erase(it);
            owner_.erase(id);
            edges_.erase(id);
            for (auto &[_, outs] : edges_) outs.erase(id);
        }
    }
    doomed.clear(); // 出了锁才 join（见文件头）
}

} // namespace realagent
