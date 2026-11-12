# respond_to? asks the program's respond_to_missing? when it finds no
# method, with include_all as true or false, and answers true or false.
class R
  def respond_to_missing?(n, p = false) = n == :dyn || (p && n == :hid)
end
class Q
  private
  def respond_to_missing?(n, p = false)
    $log << [n, p]
    n.to_s.start_with?("q") ? "yes" : nil
  end
end
$log = []
class S < R
  def real = 1
  def ask = respond_to?(:dyn)
  def ask2 = respond_to?(:other)
end
module M
  def respond_to_missing?(n, ip) = n == :mm || super
end
class T; include M; end
class R
  def self.cm = respond_to?(:dyn)
  def self.cm2 = self.respond_to?(:dyn)
end
cnt = 0
mk = -> { cnt += 1; Q.new }
p R.new.respond_to?(:dyn), R.new.respond_to?("dyn"), R.new.respond_to?(:hid), R.new.respond_to?(:hid, true)
ia = [true, false].first
p R.new.respond_to?(:hid, ia)
p mk.().respond_to?(:qx), cnt, mk.().respond_to?(:zz, true), cnt
p $log
p S.new.respond_to?(:real), S.new.ask, S.new.ask2, S.new.respond_to?(:dyn)
p T.new.respond_to?(:mm), T.new.respond_to?(:nn)
p R.cm, R.cm2, R.respond_to?(:dyn)
p 5.respond_to?(:dyn), "s".respond_to?(:upcase), nil.respond_to?(:dyn)
[R.new, Q.new, 3, "x", T.new, nil].each { |v| p [v.respond_to?(:dyn), v.respond_to?(:qa), v.respond_to?(:mm)] }
p $log.size

# a call that runs before the def answers by the methods alone
class U; end
p U.new.respond_to?(:late)
class U
  def respond_to_missing?(n, p = false) = n == :late
end
p U.new.respond_to?(:late)
