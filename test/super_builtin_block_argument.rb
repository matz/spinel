# A super's block argument into a builtin arm that takes no block runs once,
# after the arguments: before the count check of a list the run counts (an
# empty rest into respond_to? raises after it ran), and in the class arms,
# include?, superclass, subclasses and ancestors, too, and before the
# ArgumentError of a count the builtin never takes.
$log = []
def make_block = ($log << :blk; proc {})
module N; end
class K
  include N
  def self.include?(m) = super(m, &make_block)
  def self.superclass = super(&make_block)
  def self.subclasses = super(&make_block)
  def self.ancestors(*a) = super(*a, &make_block)
  def is_a?(*a) = super(*a, &make_block)
  def respond_to?(*a) = super(*a, &make_block)
end
p K.include?(N), $log
p K.superclass, $log
p K.subclasses, $log.size
p K.ancestors.first, $log
begin; K.ancestors(1); rescue ArgumentError => e; p e.message; end
p $log
begin; K.new.is_a?; rescue ArgumentError => e; p e.message; end
p $log, K.new.is_a?(K), $log
p K.new.respond_to?(:is_a?), $log
class R
  def respond_to?(*a) = super(*a, &make_block)
end
begin; R.new.respond_to?; rescue ArgumentError => e; p e.message; end
p $log.size
# a count the builtin never takes: the arguments run, then the block
# argument, then the ArgumentError
$log.clear
def lg(x) = ($log << x; x)
class C
  def self.superclass = super(lg(1), &make_block)
  def self.ancestors = super(lg(2), 3, &make_block)
  def is_a? = super(lg(C), lg(4), &make_block)
  def instance_of? = super(&make_block)
end
begin; C.superclass; rescue ArgumentError => e; p e.message; end
p $log; $log.clear
begin; C.ancestors; rescue ArgumentError => e; p e.message; end
p $log; $log.clear
begin; C.new.is_a?; rescue ArgumentError => e; p e.message; end
p $log; $log.clear
begin; C.new.instance_of?; rescue ArgumentError => e; p e.message; end
p $log
