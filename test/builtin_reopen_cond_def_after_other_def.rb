# A def under a bare method_defined? in a builtin's reopening, with another
# class's def of the name ahead of it: the condition asks the builtin, whose
# method CRuby has, so the def does not run.
class Stamp
  def to_a = [:stamp]
end
class Time
  def to_a
    [:mine]
  end unless method_defined?(:to_a)
  def to_stamp
    Stamp.new
  end unless method_defined?(:to_stamp)
end
t = Time.utc(2026, 1, 2, 3, 4, 5)
p t.to_a.size, t.to_stamp.to_a
