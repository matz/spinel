class Ev
  attr_reader :log
  def initialize = @log = []
  def context(&) = instance_eval(&)
  def note(x) = @log << x
end
class Machine
  def initialize = @ev = Ev.new
  def event(&) = @ev.context(&)
end
class St
  def call(o, m = :event, *args, &) = o.send(m, *args, &)
end
class Cb
  def call(o, *) = yield
end
class T
  def initialize(cbs) = @cbs = cbs
  def hello = "hello from T"
  def go(cb) = cb.call(self) { hello }
  def before(index = 0, &)
    cb = @cbs[index]
    if cb
      cb.call(self) { before(index + 1, &) }
    else
      yield
    end
  end
end
p T.new([]).go([Cb.new, St.new].first)
p T.new([Cb.new, Cb.new, nil]).before([0, "x"].first) { :done }
p T.new([St.new, nil].drop(1)).before { :skip }
p St.new.call(Machine.new) { note(1) }
