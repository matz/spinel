# spinel: rbs-seed-run
# Typed Hash and Array parameter defaults (empty {}, {}.freeze, Hash.new,
# narrower String-keyed literals, helper-returned hashes, nilable hashes,
# empty-hash locals, constants, and typed arrays) coerce or adopt the seeded
# slot cleanly while preserving nil, frozen state, and shared mutation.

HDSC_EMPTY = {}
HDSC_FROZEN = {}.freeze

class HdscRouter
  def default_str_hash
    { "d" => "1" }
  end

  def maybe_hash(flag)
    flag ? { "m" => "yes" } : nil
  end

  def link_empty(path, params: {})
    q = params.map { |k, v| "#{k}=#{v}" }.join("&")
    q.empty? ? path : "#{path}?#{q}"
  end

  def link_str_lit(path, params: { "a" => "1" })
    params["p"] = path
    params["n"] = path.size
    params
  end

  def link_int_lit(path, params: { "a" => 1 })
    params["p"] = path
    params
  end

  def link_cleared(path, params: (h = { "" => "" }; h.clear; h))
    params["p"] = path
    params
  end

  def link_helper(path, params: default_str_hash)
    params["p"] = path
    params
  end

  def link_nilable(flag, params: maybe_hash(flag))
    params.nil? ? "nil" : params["m"].to_s
  end

  def link_str_str(path, params: {}.freeze)
    [path, params.size, params.frozen?]
  end

  def link_local_empty(params: (h = {}; h))
    params.size
  end

  def mut_const(k, params: HDSC_EMPTY)
    params[k] = 1
    params
  end

  def frozen_const(params: HDSC_FROZEN)
    params.frozen?
  end

  def join_frozen(items: [].freeze)
    [items.join(","), items.frozen?]
  end

  def count_poly(items: ["a"])
    items.size
  end
end

class HdscSubRouter < HdscRouter
end

class HdscBox
  def initialize
    @h = { "k" => "v" }
  end

  def h
    @h
  end
end

r = HdscSubRouter.new
puts r.link_empty("/a")
puts r.link_empty("/b", params: { "x" => 2 })
p r.link_str_lit("/a")
p r.link_str_lit("/b", params: { "x" => 2 })
p r.link_int_lit("/c")
p r.link_int_lit("/d", params: { "x" => "y" })
p r.link_cleared("/e")
p r.link_helper("/f")
puts r.link_nilable(true)
puts r.link_nilable(false)
puts r.link_nilable(false, params: { "m" => 9 })
p r.link_str_str("/g")
p r.link_str_str("/h", params: { "k" => "v" })
p r.link_local_empty
p r.link_local_empty(params: { "a" => "b" })

res = r.mut_const("z")
p res, HDSC_EMPTY, res.equal?(HDSC_EMPTY)
p r.frozen_const, r.frozen_const(params: { "a" => 1 })

p r.join_frozen
p r.join_frozen(items: ["x", "y"])
p r.count_poly
p r.count_poly(items: [1, "b"])

p HdscBox.new.h
