# A def under a run-time condition in a reopened builtin class's body
# answers only once its branch has run; otherwise the builtin's own method
# answers, as in CRuby (#11819).
class Array
  if ENV["SPINEL_NO_SUCH_VARIABLE"]
    def size = 77
  end
end
class String
  if $stdout
    def length = 99
  end
end
class Hash
  if ENV["SPINEL_NO_SUCH_VARIABLE"]
    def size = -1
  end
end
p [1, 2].size
p "abc".length
p({a: 1, b: 2}.size)
p [1, 2, 3].map(&:to_s).size
