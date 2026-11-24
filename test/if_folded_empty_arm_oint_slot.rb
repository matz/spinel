# A conditional the compiler folds, whose live arm is empty, answers nil in
# the form its slot takes. The other arm's value types the slot (a nullable
# Integer or Float, a String, an object), so a boxed nil does not fit it: a
# method result, a local, an instance variable, a block value, an argument.
class Pt
  def initialize(v); @v = v; end
end

class Box
  def run
    s = +"x"
    rn = (instance_exec(1) { |t| (t << 2 if t.is_a?(String)) })
    p rn
    rf = (instance_exec(1.5) { |t| (t * 2 if t.is_a?(String)) })
    p rf
    ru = (instance_exec(3) { |t| (t + 1 unless t.is_a?(Integer)) })
    p ru
    r = (instance_exec(s) { |t| (t << "!" if t.is_a?(String)) })
    p [s, r.equal?(s)]
    y = (instance_exec(:a) { |t| (t.to_s if t.is_a?(Integer)) })
    p y
    @iv = (instance_exec(1) { |t| (t + 1 if t.is_a?(String)) })
    p @iv
  end
end
Box.new.run

def int_of(t) = (t + 1 if t.is_a?(String))
def float_of(t) = (t * 2.5 if t.is_a?(String))
def pt_of(t) = (Pt.new(t) if t.is_a?(String))
def sym_of(t) = (t.to_sym if t.is_a?(Integer))
p int_of(1), float_of(2), pt_of(3), sym_of("a")
p [1, 2].map { |t| (t + 1 if t.is_a?(String)) }
l = ->(t) { (t.abs if t.is_a?(String)) }
p l.call(2.5)
n = (case 1 when 1 then (4 if 1.is_a?(String)) end)
p n
p [int_of(5), float_of(6)]
