# frozen_string_literal: true
# spinel: gc-minor
# spinel: gc-stress
# A Struct conversion keeps each key alive and evaluates it before the
# value, including a value that collects or changes that key.

St = Struct.new(:aa, :bb)

puts "explicit_gc"
o = St.new(+"x", +"y")
r = o.to_h { |k, v| [k.to_s + "!", (GC.start; v + "!")] }
p r

puts "alias_mutation"
s = +"key"
r = St.new(1, 2).to_h { |k, v| [s, (s << "!"; GC.start; v)] }
p r
p s

puts "reassign_key"
s = +"old"
r = St.new(1, 2).to_h { |k, v| [(s = k.to_s + "!"), (s = +"new"; GC.start; v)] }
p r

puts "order"
def ordered_key(k)
  puts "key #{k}"
  k.to_s + "!"
end
def ordered_value(v)
  puts "value #{v}"
  GC.start
  [v, v + 1]
end
p St.new(1, 2).to_h { |k, v| [ordered_key(k), ordered_value(v)] }

puts "raise_order"
def raising_key(k)
  puts k
  raise "key"
end
def raising_value(v)
  puts v
  raise "value"
end
begin
  St.new(1, 2).to_h { |k, v| [raising_key(k), raising_value(v)] }
rescue RuntimeError => e
  puts e.message
end
begin
  St.new(1, 2).to_h { |k, v| [(puts k; k.to_s + "!"), raising_value(v)] }
rescue RuntimeError => e
  puts e.message
end

puts "composite"
p St.new(1, 2).to_h { |k, v| [[k.to_s + "!"], (GC.start; {v => v + 1})] }

puts "leading_stmts"
p St.new(1, 2).to_h { |k, v| puts k; [k.to_s + "!", (GC.start; v.to_s)] }

puts "null_value"
p St.new(nil, +"value").to_h { |k, v| [k.to_s + "!", (GC.start; v)] }

puts "boxed_alias"
def choose(s, k)
  k == :aa ? s : 1
end
s = +"key"
r = St.new(1, 2).to_h { |k, v| [choose(s, k), (s << "!"; GC.start; v)] }
p r
p s

puts "realloc_key"
s = +"key"
r = St.new(1, 2).to_h { |k, v| [s, (s << "!" * 1000; GC.start; v)] }
p r.keys.map { |k| [k.size, k[0, 3]] }
p r.values
