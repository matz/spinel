# Compiled with --defer-refusals: an attr call whose splat the compiler
# cannot read (a constant or a local changed before it, directly or through
# the Array its to_a answers) would declare nothing, so it is refused,
# and raises NotImplementedError where it stands in the class body, after
# the statements ahead of it. Without the flag it is refused.
# spinel: defer-refusals: 4:start body true true true after
puts "start"
begin
  class C
    FLAGS = %i[a b]
    FLAGS << :c
    puts "body"
    attr_accessor(*FLAGS)
  end
rescue NotImplementedError => e
  puts e.message.include?("attr_accessor with a splat")
end
begin
  class D
    NAMES = %i[a]
    NAMES.to_a << :b
    attr_reader(*NAMES)
  end
rescue NotImplementedError => e
  puts e.message.include?("attr_reader with a splat")
end
begin
  class E
    names = [:q]
    names << :r
    attr_reader(*names)
  end
rescue NotImplementedError => e
  puts e.message.include?("attr_reader with a splat")
end
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
