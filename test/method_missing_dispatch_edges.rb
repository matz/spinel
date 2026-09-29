# A call nothing in the program defines reaches method_missing on a receiver
# whose class has one; elsewhere it stays NoMethodError.
class Rec
  attr_reader :log
  def initialize = @log = []
  def method_missing(name, *args, &blk)
    return super unless name.to_s.start_with?("rec_")
    @log << [name, args, blk ? blk.call : nil]
    self
  end
  def respond_to_missing?(name, priv = false) = name.to_s.start_with?("rec_")
end
class Plain; end
r = Rec.new
r.rec_a(1).rec_b { 7 }
p r.log
p r.respond_to?(:rec_x), r.respond_to?(:other), r.respond_to?(:rec_y, true)
begin
  r.other_thing
rescue NoMethodError => e
  p e.name
end
begin
  Plain.new.rec_a
rescue NoMethodError => e
  p e.name
end
# an attr_writer is a defined method, not the hook's
class Cfg
  attr_accessor :size
  def method_missing(name, *args) = :hook
end
c = Cfg.new
c.size = 4
p c.size, c.other
# a private respond_to_missing? is consulted too
class Priv
  private def respond_to_missing?(name, all = false) = name == :ghost
end
pv = Priv.new
p pv.respond_to?(:ghost), pv.respond_to?(:real)
