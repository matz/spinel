# spinel: share
# Empty {} and [] parameter defaults adopt the parameter's settled container
# variant without widening to poly when non-empty call sites agree, widen
# cleanly when mutated with other value types, and preserve frozen state.

def link(path, params: {})
  q = params.map { |k, v| "#{k}=#{v}" }.join("&")
  q.empty? ? path : "#{path}?#{q}"
end

def count_all(counts: {})
  sum = 0
  counts.each { |k, v| sum += v }
  sum
end

def make_params
  { "x" => "1", "y" => "2" }
end

def link_indirect(path, params: {})
  q = params.map { |k, v| "#{k}=#{v}" }.join("&")
  q.empty? ? path : "#{path}?#{q}"
end

def link_frozen(path, params: {}.freeze)
  [path, params.size, params.frozen?]
end

def fill_mutated(h = {})
  h["k"] = 1
  h
end

def fill_mixed_default(params: { "a" => "1" })
  params["b"] = 2
  params
end

def join_all(items: [])
  items.join(",")
end

def join_frozen(items: [].freeze)
  [items.join(","), items.frozen?]
end

module UrlHelpers
  def build_url(path, params: {})
    q = params.map { |k, v| "#{k}=#{v}" }.join("&")
    q.empty? ? path : "#{path}?#{q}"
  end
end

class StrPage
  include UrlHelpers
end

class IntPage
  include UrlHelpers
end

puts link("/a")
puts link("/b", params: { "x" => "1" })
puts count_all
puts count_all(counts: { "a" => 1, "b" => 2 })
puts link_indirect("/c")
puts link_indirect("/d", params: make_params)
p link_frozen("/e")
p link_frozen("/f", params: { "k" => "v" })

p fill_mutated
caller_h = { "a" => "b" }
p fill_mutated(caller_h)
p caller_h

p fill_mixed_default
p fill_mixed_default(params: { "x" => 1, "y" => "2" })

puts join_all
puts join_all(items: ["a", "b"])
p join_frozen
p join_frozen(items: ["x"])

puts StrPage.new.build_url("/s1")
puts StrPage.new.build_url("/s2", params: { "k" => "v" })
puts IntPage.new.build_url("/i1")
puts IntPage.new.build_url("/i2", params: { "n" => 7 })
