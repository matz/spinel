# A reopening of a builtin class that a run-time condition holds: the class
# is there either way, so its def answers only once the branch has run;
# otherwise the builtin's own method answers, or none, as in CRuby (#11819).
if ENV["SPINEL_NO_SUCH_VARIABLE"]
  class Array
    def size = 77
  end
end
if $stdout
  class String
    def length = 99
  end
end
unless $stdout
  class Hash
    def size = -1
  end
end
if ENV["SPINEL_NO_SUCH_VARIABLE"]
  class String
    def shout = upcase
  end
end
p [1, 2].size
p "abc".length
p({a: 1, b: 2}.size)
p [1, 2, 3].map(&:to_s).size
begin
  p "a".shout
rescue NoMethodError
  puts "nomethod"
end
