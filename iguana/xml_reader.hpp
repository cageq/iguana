#pragma once
#include "reflection.hpp"
#include "type_traits.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <msstl/charconv.hpp>
#include <optional>
#include <rapidxml.hpp>
#include <string>
#include <type_traits>

namespace iguana {
inline std::string g_xml_read_err;
template <typename T> void do_read(rapidxml::xml_node<char> *node, T &&t);

constexpr inline size_t find_underline(const char *str) {
  const char *c = str;
  for (; *c != '\0'; ++c) {
    if (*c == '_') {
      break;
    }
  }
  return c - str;
}

template <typename T> inline void missing_node_handler(std::string_view name) {
  std::cout << name << " not found\n";
  if (iguana::is_required<T>(name)) {
    std::string err = "required filed ";
    err.append(name).append(" not found!");
    throw std::invalid_argument(err);
  }
}

template <typename T> inline void parse_num(T &num, std::string_view value) {
  if (value.empty()) {
    return;
  }

  if constexpr (std::is_arithmetic_v<T>) {
    auto [p, ec] =
        msstl::from_chars(value.data(), value.data() + value.size(), num);
#if defined(_MSC_VER)
    if (ec != std::errc{})
#else
    if (__builtin_expect(ec != std::errc{}, 0))
#endif
      throw std::invalid_argument("Failed to parse number");
  } else {
    static_assert(!sizeof(T), "don't support this type");
  }
}

class any_t {
public:
  explicit any_t(std::string_view value = "") : value_(value) {}
  template <typename T> std::pair<bool, T> get() const {
    if constexpr (std::is_same_v<T, std::string> ||
                  std::is_same_v<T, std::string_view>) {
      return std::make_pair(true, T{value_});
    } else if constexpr (std::is_arithmetic_v<T>) {
      T num{};
      try {
        parse_num<T>(num, value_);
        return std::make_pair(true, static_cast<T>(num));
      } catch (std::exception &e) {
        g_xml_read_err = e.what();
        std::cout << "parse num failed, reason: " << e.what() << "\n";
        return std::make_pair(false, T{});
      }
    } else {
      static_assert(!sizeof(T), "don't support this type!!");
    }
  }

  std::string_view get_value() const { return value_; }

private:
  std::string_view value_;
};

template <typename T> class namespace_t {
public:
  using value_type = T;
  explicit namespace_t(T &&value) : value_(std::forward<T>(value)) {}
  namespace_t() : value_() {}
  const T &get() const { return value_; }

private:
  T value_;
};

class cdata_t {
public:
  cdata_t() : value_() {}
  cdata_t(const char *c, size_t len) : value_(c, len) {}
  std::string_view get() const { return value_; }

private:
  std::string_view value_;
};

template <typename T>
inline void parse_attribute(rapidxml::xml_node<char> *node, T &t) {
  using U = std::decay_t<T>;
  static_assert(is_map_container<U>::value, "must be map container");
  using key_type = typename U::key_type;
  using value_type = typename U::mapped_type;
  static_assert(is_str_v<key_type>, " key of attribute map must be str");
  rapidxml::xml_attribute<> *attr = node->first_attribute();
  while (attr != nullptr) {
    value_type value_item;
    std::string_view value(attr->value(), attr->value_size());
    if constexpr (is_str_v<value_type> || std::is_same_v<any_t, value_type>) {
      value_item = value_type{value};
    } else if constexpr (std::is_arithmetic_v<value_type> &&
                         !std::is_same_v<bool, value_type>) {
      parse_num<value_type>(value_item, value);
    } else {
      static_assert(!sizeof(value_type), "value type not supported");
    }
    t.emplace(key_type(attr->name(), attr->name_size()), std::move(value_item));
    attr = attr->next_attribute();
  }
}

inline rapidxml::xml_node<char> *
find_cdata(const rapidxml::xml_node<char> *node) {
  for (auto cn = node->first_node(); cn; cn = cn->next_sibling()) {
    if (cn->type() == rapidxml::node_cdata) {
      return cn;
    }
  }
  return nullptr;
}

template <typename T>
inline void parse_item(rapidxml::xml_node<char> *node, T &t,
                       std::string_view value) {
  using U = std::remove_reference_t<T>;
  if constexpr (std::is_same_v<char, U>) {
    if (!value.empty())
      t = value.back();
  } else if constexpr (std::is_arithmetic_v<U>) {
    if constexpr (std::is_same_v<bool, U>) {
      if (value == "true" || value == "1" || value == "True" ||
          value == "TRUE") {
        t = true;
      } else if (value == "false" || value == "0" || value == "False" ||
                 value == "FALSE") {
        t = false;
      } else {
        throw std::invalid_argument("Failed to parse bool");
      }
    } else {
      parse_num<U>(t, value);
    }
  } else if constexpr (is_str_v<U>) {
    t = U{value};
  } else if constexpr (is_reflection_v<U>) {
    do_read(node, t);
  } else if constexpr (is_std_optinal_v<U>) {
    if (!value.empty()) {
      using value_type = typename U::value_type;
      value_type opt{};
      parse_item(node, opt, value);
      t = std::move(opt);
    }
  } else if constexpr (is_std_pair_v<U>) {
    parse_item(node, t.first, value);
    parse_attribute(node, t.second);
  } else if constexpr (is_namespace_v<U>) {
    using value_type = typename U::value_type;
    value_type ns{};
    if constexpr (is_reflection_v<value_type>) {
      do_read(node, ns);
    } else {
      parse_item(node, ns, value);
    }
    t = T{std::move(ns)};
  } else {
    static_assert(!sizeof(T), "don't support this type!!");
  }
}

template <typename T, typename member_type>
inline void parse_node(rapidxml::xml_node<char> *node, member_type &&t,
                       std::string_view name) {
  using item_type = std::decay_t<member_type>;
  if constexpr (std::is_same_v<cdata_t, item_type>) {
    auto c_node = find_cdata(node);
    if (c_node) {
      t = cdata_t(c_node->value(), c_node->value_size());
    }
  } else if constexpr (
      is_std_optinal_v<item_type>) { // std::optional<std::vector<some_object>>
    using op_value_type = typename item_type::value_type;
    if constexpr ((!is_str_v<op_value_type> &&
                   is_container<op_value_type>::value) ||
                  std::is_same_v<cdata_t, op_value_type>) {
      op_value_type op_val;
      parse_node<T>(node, op_val, name);
      t = std::move(op_val);
    } else {
      auto n = node->first_node(name.data());
      if (n) {
        parse_item(n, t, std::string_view(n->value(), n->value_size()));
      }
    }
  } else if constexpr (!is_str_v<item_type> && is_container<item_type>::value) {
    using value_type = typename item_type::value_type;
    if constexpr (std::is_same_v<cdata_t, value_type>) {
      for (auto c = node->first_node(); c; c = c->next_sibling()) {
        if (c->type() == rapidxml::node_cdata) {
          t.push_back(value_type(c->value(), c->value_size()));
        }
      }
    } else {
      auto n = node->first_node(name.data());
      if (n) {
        while (n) {
          if (std::string_view(n->name(), n->name_size()) != name) {
            break;
          }
          value_type item;
          parse_item(n, item, std::string_view(n->value(), n->value_size()));
          t.push_back(std::move(item));
          n = n->next_sibling();
        }
      } else {
        if constexpr (!is_std_optinal_v<value_type>) {
          missing_node_handler<T>(name);
        }
      }
    }
  } else {
    auto n = node->first_node(name.data());
    if (n) {
      parse_item(n, t, std::string_view(n->value(), n->value_size()));
    } else {
      if constexpr (!is_std_optinal_v<item_type>) {
        missing_node_handler<T>(name);
      }
    }
  }
}

template <typename T>
inline void do_read(rapidxml::xml_node<char> *node, T &&t) {
  static_assert(is_reflection_v<std::remove_reference_t<T>>,
                "must be refletable object");
  for_each(std::forward<T>(t), [&t, &node](const auto member_ptr, auto i) {
    using member_ptr_type = std::decay_t<decltype(member_ptr)>;
    using type_v =
        decltype(std::declval<T>().*std::declval<decltype(member_ptr)>());
    using item_type = std::decay_t<type_v>;

    if constexpr (std::is_member_pointer_v<member_ptr_type>) {
      using M = decltype(iguana_reflect_members(std::forward<T>(t)));
      constexpr auto Idx = decltype(i)::value;
      constexpr auto Count = M::value();
      static_assert(Idx < Count);
      constexpr auto key = M::arr()[Idx];
      std::string_view str = key.data();
      if constexpr (is_map_container<item_type>::value) {
        parse_attribute(node, t.*member_ptr);
      } else if constexpr (is_namespace_v<item_type>) {
        constexpr auto index_ul = find_underline(key.data());
        static_assert(index_ul < key.size(),
                      "'_' is needed in namesapce_t value name");
        std::string ns(key.data(), key.size());
        ns[index_ul] = ':';
        parse_node<T>(node, t.*member_ptr, ns);
      } else {
        parse_node<T>(node, t.*member_ptr, str);
      }
    } else {
      static_assert(!sizeof(member_ptr_type), "type not supported");
    }
  });
}

template <int Flags = 0, typename T,
          typename = std::enable_if_t<is_reflection<T>::value>>
inline bool from_xml(T &&t, char *buf) {
  if (!g_xml_read_err.empty()) {
    g_xml_read_err.clear();
  }
  try {
    rapidxml::xml_document<> doc;
    doc.parse<Flags>(buf);

    auto fisrt_node = doc.first_node();
    if (fisrt_node)
      do_read(fisrt_node, t);

    return true;
  } catch (std::exception &e) {
    g_xml_read_err = e.what();
    std::cout << e.what() << "\n";
  }

  return false;
}

inline std::string get_last_read_err() { return g_xml_read_err; }
} // namespace iguana
