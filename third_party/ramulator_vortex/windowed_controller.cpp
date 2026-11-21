// Ramulator's generic DRAM controller (src/dram_controller/impl/
// generic_dram_controller.cpp of the pinned submodule) under the name
// "Windowed", with two changes:
//  - the read and write buffer sizes are parameters (constants there);
//  - with the FRFCFS scheduler and a device whose prerequisites depend on its
//    state only (HBM2, HBM3, GDDR6), the scheduling is the same choice made
//    with less work. A request's prerequisite command changes only when this
//    controller issues a command (the device has no other state change), so
//    it is kept until the next issue; whether a command is ready depends on
//    the command and the nodes down to its bank only, so it is asked once per
//    (command, bank) and cycle; and a read or write buffer holds its requests
//    in arrival order, so FRFCFS's choice in it is the first ready request,
//    or the oldest if none is.
// The choices, the commands issued and every statistic are those of the
// generic controller.

#include "dram_controller/controller.h"
#include "memory_system/memory_system.h"

#include <cstring>

namespace Ramulator {

class WindowedDRAMController final : public IDRAMController, public Implementation {
  RAMULATOR_REGISTER_IMPLEMENTATION(IDRAMController, WindowedDRAMController, "Windowed", "The generic DRAM controller with sized request buffers.");
  private:
    // Prerequisite caching and the ready memo (see the top of the file).
    bool m_fast = false;
    // Commands issued so far, plus one: a request's cached prerequisite is
    // valid while the epoch it was computed at (in its scratchpad) is current.
    uint64_t m_epoch = 1;
    int m_bank_levels = 0;              // levels from the channel down to the bank
    std::vector<int> m_level_count;     // nodes per parent at each of those levels
    int m_num_commands = 0;
    std::vector<Clk_t> m_ready_clk;     // per (bank, command): the cycle of the memo, -1 none
    std::vector<uint64_t> m_ready_epoch;
    std::vector<char> m_ready_val;

    std::deque<Request> pending;          // A queue for read requests that are about to finish (callback after RL)

    ReqBuffer m_active_buffer;            // Buffer for requests being served. This has the highest priority
    ReqBuffer m_priority_buffer;          // Buffer for high-priority requests (e.g., maintenance like refresh).
    ReqBuffer m_read_buffer;              // Read request buffer
    ReqBuffer m_write_buffer;             // Write request buffer

    int m_bank_addr_idx = -1;

    float m_wr_low_watermark;
    float m_wr_high_watermark;
    bool  m_is_write_mode = false;

    size_t s_row_hits = 0;
    size_t s_row_misses = 0;
    size_t s_row_conflicts = 0;
    size_t s_read_row_hits = 0;
    size_t s_read_row_misses = 0;
    size_t s_read_row_conflicts = 0;
    size_t s_write_row_hits = 0;
    size_t s_write_row_misses = 0;
    size_t s_write_row_conflicts = 0;

    size_t m_num_cores = 0;
    std::vector<size_t> s_read_row_hits_per_core;
    std::vector<size_t> s_read_row_misses_per_core;
    std::vector<size_t> s_read_row_conflicts_per_core;

    size_t s_num_read_reqs = 0;
    size_t s_num_write_reqs = 0;
    size_t s_num_other_reqs = 0;
    size_t s_queue_len = 0;
    size_t s_read_queue_len = 0;
    size_t s_write_queue_len = 0;
    size_t s_priority_queue_len = 0;
    float s_queue_len_avg = 0;
    float s_read_queue_len_avg = 0;
    float s_write_queue_len_avg = 0;
    float s_priority_queue_len_avg = 0;

    size_t s_read_latency = 0;
    float s_avg_read_latency = 0;


  public:
    void init() override {
      m_wr_low_watermark =  param<float>("wr_low_watermark").desc("Threshold for switching back to read mode.").default_val(0.2f);
      m_wr_high_watermark = param<float>("wr_high_watermark").desc("Threshold for switching to write mode.").default_val(0.8f);
      m_read_buffer.max_size = param<int>("read_buffer_size").desc("Read requests the scheduler chooses among.").default_val(32);
      m_write_buffer.max_size = param<int>("write_buffer_size").desc("Write requests the scheduler chooses among.").default_val(32);

      m_scheduler = create_child_ifce<IScheduler>();
      m_refresh = create_child_ifce<IRefreshManager>();
      m_rowpolicy = create_child_ifce<IRowPolicy>();

      if (m_config["plugins"]) {
        YAML::Node plugin_configs = m_config["plugins"];
        for (YAML::iterator it = plugin_configs.begin(); it != plugin_configs.end(); ++it) {
          m_plugins.push_back(create_child_ifce<IControllerPlugin>(*it));
        }
      }
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
      m_dram = memory_system->get_ifce<IDRAM>();
      m_bank_addr_idx = m_dram->m_levels("bank");
      m_priority_buffer.max_size = 512*3 + 32;

      this->setup_fast();

      m_num_cores = frontend->get_num_cores();

      s_read_row_hits_per_core.resize(m_num_cores, 0);
      s_read_row_misses_per_core.resize(m_num_cores, 0);
      s_read_row_conflicts_per_core.resize(m_num_cores, 0);

      register_stat(s_row_hits).name("row_hits_{}", m_channel_id);
      register_stat(s_row_misses).name("row_misses_{}", m_channel_id);
      register_stat(s_row_conflicts).name("row_conflicts_{}", m_channel_id);
      register_stat(s_read_row_hits).name("read_row_hits_{}", m_channel_id);
      register_stat(s_read_row_misses).name("read_row_misses_{}", m_channel_id);
      register_stat(s_read_row_conflicts).name("read_row_conflicts_{}", m_channel_id);
      register_stat(s_write_row_hits).name("write_row_hits_{}", m_channel_id);
      register_stat(s_write_row_misses).name("write_row_misses_{}", m_channel_id);
      register_stat(s_write_row_conflicts).name("write_row_conflicts_{}", m_channel_id);

      for (size_t core_id = 0; core_id < m_num_cores; core_id++) {
        register_stat(s_read_row_hits_per_core[core_id]).name("read_row_hits_core_{}", core_id);
        register_stat(s_read_row_misses_per_core[core_id]).name("read_row_misses_core_{}", core_id);
        register_stat(s_read_row_conflicts_per_core[core_id]).name("read_row_conflicts_core_{}", core_id);
      }

      register_stat(s_num_read_reqs).name("num_read_reqs_{}", m_channel_id);
      register_stat(s_num_write_reqs).name("num_write_reqs_{}", m_channel_id);
      register_stat(s_num_other_reqs).name("num_other_reqs_{}", m_channel_id);
      register_stat(s_queue_len).name("queue_len_{}", m_channel_id);
      register_stat(s_read_queue_len).name("read_queue_len_{}", m_channel_id);
      register_stat(s_write_queue_len).name("write_queue_len_{}", m_channel_id);
      register_stat(s_priority_queue_len).name("priority_queue_len_{}", m_channel_id);
      register_stat(s_queue_len_avg).name("queue_len_avg_{}", m_channel_id);
      register_stat(s_read_queue_len_avg).name("read_queue_len_avg_{}", m_channel_id);
      register_stat(s_write_queue_len_avg).name("write_queue_len_avg_{}", m_channel_id);
      register_stat(s_priority_queue_len_avg).name("priority_queue_len_avg_{}", m_channel_id);

      register_stat(s_read_latency).name("read_latency_{}", m_channel_id);
      register_stat(s_avg_read_latency).name("avg_read_latency_{}", m_channel_id);
    };

    bool send(Request& req) override {
      req.final_command = m_dram->m_request_translations(req.type_id);

      switch (req.type_id) {
        case Request::Type::Read: {
          s_num_read_reqs++;
          break;
        }
        case Request::Type::Write: {
          s_num_write_reqs++;
          break;
        }
        default: {
          s_num_other_reqs++;
          break;
        }
      }

      // Forward existing write requests to incoming read requests
      if (req.type_id == Request::Type::Read) {
        auto compare_addr = [req](const Request& wreq) {
          return wreq.addr == req.addr;
        };
        if (std::find_if(m_write_buffer.begin(), m_write_buffer.end(), compare_addr) != m_write_buffer.end()) {
          // The request will depart at the next cycle
          req.depart = m_clk + 1;
          pending.push_back(req);
          return true;
        }
      }

      // Else, enqueue them to corresponding buffer based on request type id
      bool is_success = false;
      req.arrive = m_clk;
      if        (req.type_id == Request::Type::Read) {
        is_success = m_read_buffer.enqueue(req);
      } else if (req.type_id == Request::Type::Write) {
        is_success = m_write_buffer.enqueue(req);
      } else {
        throw std::runtime_error("Invalid request type!");
      }
      if (!is_success) {
        // We could not enqueue the request
        req.arrive = -1;
        return false;
      }

      return true;
    };

    bool priority_send(Request& req) override {
      req.final_command = m_dram->m_request_translations(req.type_id);

      bool is_success = false;
      is_success = m_priority_buffer.enqueue(req);
      return is_success;
    }

    void tick() override {
      m_clk++;
      if (!m_dram->m_future_actions.empty()) {
        // A device that changes its state on its own: nothing is kept.
        ++m_epoch;
      }

      // Update statistics
      s_queue_len += m_read_buffer.size() + m_write_buffer.size() + m_priority_buffer.size() + pending.size();
      s_read_queue_len += m_read_buffer.size() + pending.size();
      s_write_queue_len += m_write_buffer.size();
      s_priority_queue_len += m_priority_buffer.size();

      // 1. Serve completed reads
      serve_completed_reads();

      m_refresh->tick();

      // 2. Try to find a request to serve.
      ReqBuffer::iterator req_it;
      ReqBuffer* buffer = nullptr;
      bool request_found = schedule_request(req_it, buffer);

      // 2.1 Take row policy action
      m_rowpolicy->update(request_found, req_it);

      // 3. Update all plugins
      for (auto plugin : m_plugins) {
        plugin->update(request_found, req_it);
      }

      // 4. Finally, issue the commands to serve the request
      if (request_found) {
        // If we find a real request to serve
        if (req_it->is_stat_updated == false) {
          update_request_stats(req_it);
        }
        m_dram->issue_command(req_it->command, req_it->addr_vec);
        ++m_epoch;

        // If we are issuing the last command, set depart clock cycle and move the request to the pending queue
        if (req_it->command == req_it->final_command) {
          if (req_it->type_id == Request::Type::Read) {
            req_it->depart = m_clk + m_dram->m_read_latency;
            pending.push_back(*req_it);
          } else if (req_it->type_id == Request::Type::Write) {
            // TODO: Add code to update statistics
          }
          buffer->remove(req_it);
        } else {
          if (m_dram->m_command_meta(req_it->command).is_opening) {
            m_active_buffer.enqueue(*req_it);
            buffer->remove(req_it);
          }
        }

      }

    };


  private:
    /**
     * @brief    Helper function to check if a request is hitting an open row
     * @details
     *
     */
    bool is_row_hit(ReqBuffer::iterator& req)
    {
        return m_dram->check_rowbuffer_hit(req->final_command, req->addr_vec);
    }
    /**
     * @brief    Helper function to check if a request is opening a row
     * @details
     *
    */
    bool is_row_open(ReqBuffer::iterator& req)
    {
        return m_dram->check_node_open(req->final_command, req->addr_vec);
    }

    /**
     * @brief
     * @details
     *
     */
    void update_request_stats(ReqBuffer::iterator& req)
    {
      req->is_stat_updated = true;

      if (req->type_id == Request::Type::Read)
      {
        if (is_row_hit(req)) {
          s_read_row_hits++;
          s_row_hits++;
          if (req->source_id != -1)
            s_read_row_hits_per_core[req->source_id]++;
        } else if (is_row_open(req)) {
          s_read_row_conflicts++;
          s_row_conflicts++;
          if (req->source_id != -1)
            s_read_row_conflicts_per_core[req->source_id]++;
        } else {
          s_read_row_misses++;
          s_row_misses++;
          if (req->source_id != -1)
            s_read_row_misses_per_core[req->source_id]++;
        }
      }
      else if (req->type_id == Request::Type::Write)
      {
        if (is_row_hit(req)) {
          s_write_row_hits++;
          s_row_hits++;
        } else if (is_row_open(req)) {
          s_write_row_conflicts++;
          s_row_conflicts++;
        } else {
          s_write_row_misses++;
          s_row_misses++;
        }
      }
    }

    /**
     * @brief    Helper function to serve the completed read requests
     * @details
     * This function is called at the beginning of the tick() function.
     * It checks the pending queue to see if the top request has received data from DRAM.
     * If so, it finishes this request by calling its callback and poping it from the pending queue.
     */
    void serve_completed_reads() {
      if (pending.size()) {
        // Check the first pending request
        auto& req = pending[0];
        if (req.depart <= m_clk) {
          // Request received data from dram
          if (req.depart - req.arrive > 1) {
            // Check if this requests accesses the DRAM or is being forwarded.
            // TODO add the stats back
            s_read_latency += req.depart - req.arrive;
          }

          if (req.callback) {
            // If the request comes from outside (e.g., processor), call its callback
            req.callback(req);
          }
          // Finally, remove this request from the pending queue
          pending.pop_front();
        }
      };
    };


    /**
     * @brief    Checks if we need to switch to write mode
     *
     */
    void set_write_mode() {
      if (!m_is_write_mode) {
        if ((m_write_buffer.size() > m_wr_high_watermark * m_write_buffer.max_size) || m_read_buffer.size() == 0) {
          m_is_write_mode = true;
        }
      } else {
        if ((m_write_buffer.size() < m_wr_low_watermark * m_write_buffer.max_size) && m_read_buffer.size() != 0) {
          m_is_write_mode = false;
        }
      }
    };


    void setup_fast() {
      std::string scheduler = m_scheduler->m_impl->get_name();
      std::string device = m_dram->m_impl->get_name();
      bool plugins_read_only = true;
      for (auto plugin : m_plugins) {
        plugins_read_only = plugins_read_only && (plugin->m_impl->get_name() == "TraceRecorder");
      }
      m_fast = (scheduler == "FRFCFS") && plugins_read_only
            && (device == "HBM2" || device == "HBM3" || device == "GDDR6")
            && m_dram->m_levels.contains("row");
      if (!m_fast) {
        return;
      }
      // The device's nodes end at the level above the row (DRAMNodeBase).
      m_bank_levels = m_dram->m_levels("row");
      int num_banks = 1;
      for (int l = 0; l < m_bank_levels; ++l) {
        int count = m_dram->m_organization.count[l];
        m_level_count.push_back(count);
        num_banks *= count;
      }
      m_num_commands = m_dram->m_commands.size();
      m_ready_clk.assign(size_t(num_banks) * m_num_commands, -1);
      m_ready_epoch.assign(m_ready_clk.size(), 0);
      m_ready_val.assign(m_ready_clk.size(), 0);
    }

    // The epoch a request's command was computed at, in its scratchpad (FRFCFS
    // does not use it).
    static uint64_t cached_epoch(const Request& req) {
      uint64_t e;
      std::memcpy(&e, req.scratchpad.data(), sizeof(e));
      return e;
    }

    static void set_cached_epoch(Request& req, uint64_t e) {
      std::memcpy(req.scratchpad.data(), &e, sizeof(e));
    }

    // The request's prerequisite command, stored in req.command as FRFCFS does.
    int update_command(Request& req) {
      if (!m_fast) {
        req.command = m_dram->get_preq_command(req.final_command, req.addr_vec);
      } else if (cached_epoch(req) != m_epoch) {
        req.command = m_dram->get_preq_command(req.final_command, req.addr_vec);
        set_cached_epoch(req, m_epoch);
      }
      return req.command;
    }

    // check_ready of the device. Its answer depends on the command, the nodes
    // from the channel to the bank, their timing (changed by an issue) and the
    // cycle; with the timing unchanged, a ready command stays ready.
    bool check_ready(int command, const AddrVec_t& addr_vec) {
      if (!m_fast) {
        return m_dram->check_ready(command, addr_vec);
      }
      size_t idx = 0;
      for (int l = 0; l < m_bank_levels; ++l) {
        int id = addr_vec[l];
        if (id < 0 || id >= m_level_count[l]) {
          return m_dram->check_ready(command, addr_vec);
        }
        idx = idx * m_level_count[l] + id;
      }
      idx = idx * m_num_commands + command;
      if (m_ready_epoch[idx] == m_epoch && (m_ready_clk[idx] == m_clk || m_ready_val[idx])) {
        return m_ready_val[idx];
      }
      bool ready = m_dram->check_ready(command, addr_vec);
      m_ready_clk[idx] = m_clk;
      m_ready_epoch[idx] = m_epoch;
      m_ready_val[idx] = ready;
      return ready;
    }

    // FRFCFS::get_best_request: the first ready request by arrival, else the
    // oldest. In a buffer kept in arrival order (`in_order`) that is the first
    // ready one in the buffer, else its head; the requests after it keep a
    // stale command, which nothing reads before the next choice recomputes it.
    ReqBuffer::iterator get_best_request(ReqBuffer& buffer, bool in_order) {
      if (!m_fast) {
        return m_scheduler->get_best_request(buffer);
      }
      if (buffer.size() == 0) {
        return buffer.end();
      }
      if (in_order) {
        for (auto it = buffer.begin(); it != buffer.end(); ++it) {
          if (this->check_ready(this->update_command(*it), it->addr_vec)) {
            return it;
          }
        }
        return buffer.begin();
      }
      auto candidate = buffer.begin();
      bool candidate_ready = this->check_ready(this->update_command(*candidate), candidate->addr_vec);
      for (auto next = std::next(buffer.begin(), 1); next != buffer.end(); next++) {
        bool next_ready = this->check_ready(this->update_command(*next), next->addr_vec);
        bool take_next = (candidate_ready ^ next_ready) ? next_ready : !(candidate->arrive <= next->arrive);
        if (take_next) {
          candidate = next;
          candidate_ready = next_ready;
        }
      }
      return candidate;
    }

    /**
     * @brief    Helper function to find a request to schedule from the buffers.
     *
     */
    bool schedule_request(ReqBuffer::iterator& req_it, ReqBuffer*& req_buffer) {
      bool request_found = false;
      // 2.1    First, check the act buffer to serve requests that are already activating (avoid useless ACTs)
      if (req_it= this->get_best_request(m_active_buffer, false); req_it != m_active_buffer.end()) {
        if (this->check_ready(req_it->command, req_it->addr_vec)) {
          request_found = true;
          req_buffer = &m_active_buffer;
        }
      }

      // 2.2    If no requests can be scheduled from the act buffer, check the rest of the buffers
      if (!request_found) {
        // 2.2.1    We first check the priority buffer to prioritize e.g., maintenance requests
        if (m_priority_buffer.size() != 0) {
          req_buffer = &m_priority_buffer;
          req_it = m_priority_buffer.begin();
          req_it->command = m_dram->get_preq_command(req_it->final_command, req_it->addr_vec);

          request_found = m_dram->check_ready(req_it->command, req_it->addr_vec);
          if (!request_found & m_priority_buffer.size() != 0) {
            return false;
          }
        }

        // 2.2.1    If no request to be scheduled in the priority buffer, check the read and write buffers.
        if (!request_found) {
          // Query the write policy to decide which buffer to serve
          set_write_mode();
          auto& buffer = m_is_write_mode ? m_write_buffer : m_read_buffer;
          // Requests enter the read and write buffers in arrival order.
          if (req_it = this->get_best_request(buffer, true); req_it != buffer.end()) {
            request_found = this->check_ready(req_it->command, req_it->addr_vec);
            req_buffer = &buffer;
          }
        }
      }

      // 2.3 If we find a request to schedule, we need to check if it will close an opened row in the active buffer.
      if (request_found) {
        if (m_dram->m_command_meta(req_it->command).is_closing) {
          auto& rowgroup = req_it->addr_vec;
          for (auto _it = m_active_buffer.begin(); _it != m_active_buffer.end(); _it++) {
            auto& _it_rowgroup = _it->addr_vec;
            bool is_matching = true;
            for (int i = 0; i < m_bank_addr_idx + 1 ; i++) {
              if (_it_rowgroup[i] != rowgroup[i] && _it_rowgroup[i] != -1 && rowgroup[i] != -1) {
                is_matching = false;
                break;
              }
            }
            if (is_matching) {
              request_found = false;
              break;
            }
          }
        }
      }

      return request_found;
    }

    void finalize() override {
      s_avg_read_latency = (float) s_read_latency / (float) s_num_read_reqs;

      s_queue_len_avg = (float) s_queue_len / (float) m_clk;
      s_read_queue_len_avg = (float) s_read_queue_len / (float) m_clk;
      s_write_queue_len_avg = (float) s_write_queue_len / (float) m_clk;
      s_priority_queue_len_avg = (float) s_priority_queue_len / (float) m_clk;

      return;
    }

};

}   // namespace Ramulator