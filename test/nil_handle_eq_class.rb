# A slot of a builtin handle kind (Mutex, Random, Thread, Queue,
# ConditionVariable, Enumerator, OpenStruct, Dir, a mutable String) that
# holds nil is a NULL pointer: `== nil`, `!= nil` and `.class` read it as
# nil. They answered false, true and the handle's class.
require 'ostruct'

class HM;  def initialize(f); @v = Mutex.new if f; end; def v_mutex; @v; end; end
class HR;  def initialize(f); @v = Random.new(1) if f; end; def v_random; @v; end; end
class HT;  def initialize(f); @v = Thread.new { 1 } if f; end; def v_thread; @v; end; end
class HQ;  def initialize(f); @v = Queue.new if f; end; def v_queue; @v; end; end
class HC;  def initialize(f); @v = ConditionVariable.new if f; end; def v_cv; @v; end; end
class HE;  def initialize(f); @v = [1, 2].each if f; end; def v_enum; @v; end; end
class HO;  def initialize(f); @v = OpenStruct.new(a: 1) if f; end; def v_os; @v; end; end
class HD;  def initialize(f); @v = Dir.new(".") if f; end; def v_dir; @v; end; end
class HS;  def initialize(f); @v = String.new("x") if f; end; def v_str; @v << "y" if @v; @v; end; end

[false, true].each do |f|
  m = HM.new(f).v_mutex;  p [m == nil, m != nil, m.class]
  r = HR.new(f).v_random; p [r == nil, r != nil, r.class]
  t = HT.new(f).v_thread; t.join if t; p [t == nil, t != nil, t.class]
  q = HQ.new(f).v_queue;  p [q == nil, q != nil, q.class]
  c = HC.new(f).v_cv;     p [c == nil, c != nil, c.class]
  e = HE.new(f).v_enum;   p [e == nil, e != nil, e.class]
  o = HO.new(f).v_os;     p [o == nil, o != nil, o.class]
  d = HD.new(f).v_dir;   p [d == nil, d != nil, d.class]; d.close if d
  s = HS.new(f).v_str;    p [s == nil, s != nil, s.class]
end
