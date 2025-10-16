// pybind_module.cpp
//
// Python bindings for the FX CLI.
// Exposes FxCli class, parsing MCU replies into Python dicts for ease of use.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <vector>
#include <sstream>
#include <cctype>
#include <cstdint>

#include "fx_client.h"

namespace py = pybind11;


// Trim helper
static inline void trim(std::string &s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) { s.clear(); return; }
    size_t end = s.find_last_not_of(" \t\n\r");
    s = s.substr(start, end - start + 1);
}

// Split helper
static std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> out;
    std::string tmp;
    std::istringstream iss(s);
    while (std::getline(iss, tmp, delim)) {
        trim(tmp);
        if (!tmp.empty()) out.push_back(tmp);
    }
    return out;
}

static inline std::string upper_copy(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Try parse float
static bool try_parse_float(const std::string &s, double &out) {
    char *end = nullptr;
    const char *cstr = s.c_str();
    out = std::strtod(cstr, &end);
    return !(end == cstr || *end != '\0');
}

// Try parse int
static bool try_parse_int(const std::string &s, long &out) {
    char *end = nullptr;
    const char *cstr = s.c_str();
    out = std::strtol(cstr, &end, 10);
    return !(end == cstr || *end != '\0');
}

// 문자열을 dict로 파싱
// s: "STATUS;fw:1.1.0, proto:ATv1;uptime:28761;NET:up, ip:..., gw:..., mask:...;...;"
static py::dict parse_response_string(const std::string &s) {
    py::dict result;
    std::string str = s;
    trim(str);

    auto segments = split(str, ';'); // 세미콜론 단위
    for (auto &seg : segments) {
        trim(seg);
        if (seg.empty()) continue;

        size_t first_colon = seg.find(':');
        if (first_colon == std::string::npos) {
            // "STATUS;" / "OBS;" 같은 단독 토큰
            result[py::str(seg)] = py::bool_(true);
            continue;
        }

        std::string head = seg.substr(0, first_colon);
        std::string rest = seg.substr(first_colon + 1);
        trim(head); trim(rest);

        if (head.empty()) continue;
        if (rest.empty()) { // "ERRS[latest]:" 처럼 값이 비어있는 케이스
            result[py::str(head)] = py::bool_(true);
            continue;
        }

        // HEAD dict 준비
        py::dict head_dict;
        // ---- 스캐닝 유틸 ----
        auto eat_ws = [&](size_t &i) {
            while (i < rest.size() && (rest[i]==' ' || rest[i]=='\t')) ++i;
        };
        auto is_ident_char = [&](char c)->bool {
            return std::isalnum((unsigned char)c) || c=='_' || c=='[' || c==']';
        };

        size_t p = 0;
        bool first_item = true;
        while (p < rest.size()) {
            eat_ws(p);
            if (p >= rest.size()) break;

            // 콤마 구분자 스킵
            if (rest[p] == ',') { ++p; continue; }
            eat_ws(p);
            if (p >= rest.size()) break;

            // 시도1: subkey:value 형태인지 체크
            size_t save = p;
            // subkey 후보 스캔 (식별자)
            size_t k_end = p;
            while (k_end < rest.size() && is_ident_char(rest[k_end])) ++k_end;

            bool has_pair = false;
            if (k_end < rest.size() && rest[k_end] == ':') {
                has_pair = true;
            }

            if (!has_pair) {
                // subkey 없이 bare value
                size_t v_start = p;
                size_t boundary = std::string::npos;
                size_t search = p;
                while (true) {
                    size_t comma = rest.find(',', search);
                    if (comma == std::string::npos) break;
                    size_t look = comma + 1;
                    eat_ws(look);
                    // 다음이 식별자 + ':' 이면 그 콤마가 경계
                    size_t t = look;
                    while (t < rest.size() && is_ident_char(rest[t])) ++t;
                    if (t > look && t < rest.size() && rest[t] == ':') {
                        boundary = comma;
                        break;
                    }
                    search = comma + 1;
                }
                size_t v_end = (boundary == std::string::npos) ? rest.size() : boundary;
                std::string v = rest.substr(v_start, v_end - v_start);
                trim(v);

                if (!v.empty()) {
                    std::string vu = upper_copy(v);
                    const bool is_state = (vu == "UP" || vu == "DOWN");
                    if (is_state) {
                        head_dict[py::str("state")] = py::str(v);
                    } else {
                        double dv;
                        long iv;
                        if (v.find('.') == std::string::npos && try_parse_int(v, iv))
                            head_dict[py::str("value")] = py::int_(iv);
                        else if (try_parse_float(v, dv))
                            head_dict[py::str("value")] = py::float_(dv);
                        else
                            head_dict[py::str("value")] = py::str(v);
                    }
                }

                if (v_end == rest.size()) { p = v_end; break; }
                p = v_end + 1; // 콤마 다음으로
                first_item = false;
                continue;
            }

            // subkey:value 처리
            std::string subkey = rest.substr(p, k_end - p);
            trim(subkey);
            p = k_end + 1; // ':' 넘김
            eat_ws(p);
            size_t v_start = p;

            // value 경계: 다음 ",<ident>:" 패턴의 콤마 직전
            size_t boundary = std::string::npos;
            size_t search = p;
            while (true) {
                size_t comma = rest.find(',', search);
                if (comma == std::string::npos) break;
                size_t look = comma + 1;
                eat_ws(look);
                size_t t = look;
                while (t < rest.size() && is_ident_char(rest[t])) ++t;
                if (t > look && t < rest.size() && rest[t] == ':') {
                    boundary = comma;
                    break;
                }
                search = comma + 1;
            }
            size_t v_end = (boundary == std::string::npos) ? rest.size() : boundary;
            std::string value_str = rest.substr(v_start, v_end - v_start);
            trim(value_str);

            if (!value_str.empty()) {
                if (value_str.find(',') != std::string::npos) {
                    // 값 내부에 콤마 → 리스트
                    py::list arr;
                    size_t q = 0;
                    while (q < value_str.size()) {
                        size_t c = value_str.find(',', q);
                        std::string token = (c == std::string::npos)
                            ? value_str.substr(q)
                            : value_str.substr(q, c - q);
                        trim(token);
                        double dv;
                        long iv;
                        if (token.find('.') == std::string::npos && try_parse_int(token, iv))
                            arr.append(py::int_(iv));
                        else if (try_parse_float(token, dv))
                            arr.append(py::float_(dv));
                        else
                            arr.append(py::str(token));
                        if (c == std::string::npos) break;
                        q = c + 1;
                    }
                    head_dict[py::str(subkey)] = arr;
                } else {
                    double dv;
                    long iv;
                    if (value_str.find('.') == std::string::npos && try_parse_int(value_str, iv))
                        head_dict[py::str(subkey)] = py::int_(iv);
                    else if (try_parse_float(value_str, dv))
                        head_dict[py::str(subkey)] = py::float_(dv);
                    else
                        head_dict[py::str(subkey)] = py::str(value_str);
                }
            } else {
                head_dict[py::str(subkey)] = py::bool_(true);
            }

            if (v_end == rest.size()) { p = v_end; break; }
            p = v_end + 1;
            first_item = false;
        }

        result[py::str(head)] = head_dict;
    }

    return result;
}


// Parse list of motor IDs
static std::vector<uint8_t> parse_id_list(const py::object &obj) {
    std::vector<uint8_t> ids;
    if (py::isinstance<py::list>(obj)) {
        py::list lst = obj.cast<py::list>();
        for (auto &item : lst) {
            int v = item.cast<int>();
            if (v < 0 || v > 255) throw std::out_of_range("ID out of range 0..255");
            ids.push_back(static_cast<uint8_t>(v));
        }
    }
    return ids;
}

PYBIND11_MODULE(fx_cli, m) {
    m.doc() = "High level FX motor controller client using UDP AT commands";

    py::class_<FxCli>(m, "FxCli")
        .def(py::init([](const std::optional<std::string> &ip_opt,
                        const std::optional<uint16_t> &port_opt) {
            // 기본값 지정
            std::string ip = ip_opt.value_or("192.168.10.10");
            uint16_t port = port_opt.value_or(5101);

            auto* cli = new FxCli(ip, port);
            try {
                set_thread_rt_and_affinity(80, 5);
                std::cerr << "[RT] FxCli main thread set to SCHED_FIFO(80) on CPU 1\n";
            } catch (...) {
                std::cerr << "[WARN] Failed to apply RT scheduling (non-root or no CAP_SYS_NICE)\n";
            }
            return cli;
        }),
        py::arg("ip") = std::nullopt,     // 선택적 인자
        py::arg("port") = std::nullopt)   // 선택적 인자

        .def("mcu_ping", [](FxCli &self) {
            std::string resp = self.mcu_ping();
            return parse_response_string(resp); // dict
        })
        .def("mcu_whoami", [](FxCli &self) {
            std::string resp = self.mcu_whoami();
            return parse_response_string(resp); // dict
        })
        .def("motor_start", [](FxCli &self, const py::object &ids_obj) {
            auto ids = parse_id_list(ids_obj);
            return self.motor_start(ids); // bool
        }, py::arg("ids"))

        .def("motor_stop", [](FxCli &self, const py::object &ids_obj) {
            auto ids = parse_id_list(ids_obj);
            return self.motor_stop(ids); // bool
        }, py::arg("ids"))

        .def("motor_estop", [](FxCli &self, const py::object &ids_obj) {
            auto ids = parse_id_list(ids_obj);
            return self.motor_estop(ids); // bool
        }, py::arg("ids"))

        .def("motor_setzero", [](FxCli &self, const py::object &ids_obj) {
            auto ids = parse_id_list(ids_obj);
            return self.motor_setzero(ids); // bool
        }, py::arg("ids"))

        .def("operation_control", [](FxCli &self, const py::list &groups) {
            std::vector<uint8_t> ids; std::vector<float> pos, vel, kp, kd, tau;
            for (auto &item : groups) {
                py::dict d = item.cast<py::dict>();
                int id = d["id"].cast<int>();
                ids.push_back((uint8_t)id);
                pos.push_back(d["pos"].cast<float>());
                vel.push_back(d["vel"].cast<float>());
                kp.push_back(d["kp"].cast<float>());
                kd.push_back(d["kd"].cast<float>());
                tau.push_back(d["tau"].cast<float>());
            }
            return self.operation_control(ids, pos, vel, kp, kd, tau);
        }, py::arg("groups"))

        .def("req", [](FxCli &self, const py::object &ids_obj) {
            auto ids = parse_id_list(ids_obj);
            std::string resp = self.req(ids);
            return parse_response_string(resp); // dict
        }, py::arg("ids"))

        .def("status", [](FxCli &self) {
            std::string resp = self.status();
            return parse_response_string(resp); // dict
        });
}
