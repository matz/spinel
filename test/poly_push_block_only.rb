# push, append and unshift with no argument but a block, on a boxed
# receiver: these methods never call the block, and an Array answers
# itself, as in CRuby.
b = proc { puts "never called" }
x = [[1], "s", {a: 1}].first
p x.push(&b)
p x.append(&b)
p x.unshift(&b)
p x.push { puts "never called" }
p x.append(&nil)
p x.unshift(&-> { 1 })
@blk = proc { }
p x.push(&@blk)

s = [[1], "s"][1]
begin
  s.push(&b)
rescue NoMethodError => e
  puts e.message
end

f = [[1].freeze, "s"].first
begin
  f.push(&b)
rescue FrozenError => e
  p e.class
end

def grow(v, &blk) = v.push(&blk)
p grow([[2], "t"].first) { }
p grow([[3], "t"].first)
