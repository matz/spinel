# A blockless any? or none? on a boxed value beside a class that defines the
# name: the dispatch's default arm answers every builtin value, and asks
# whether an element is truthy, as the boxed call's own arm does -- not
# whether there is one ([nil].any? is false, an Integer Range's true).
class Quiet
  def any? = :quiet_any
  def none? = :quiet_none
end

[[nil], [nil, 1], [], [false, nil], (1..3), (1...1), {}, {a: 1}, Quiet.new].each do |x|
  p [x.any?, x.none?]
end
begin
  [3, Quiet.new].first.any?
rescue NoMethodError => e
  p e.class
end
