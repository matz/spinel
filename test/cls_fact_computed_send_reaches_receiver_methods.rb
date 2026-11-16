# A send of a computed name reaches the methods of its receiver's classes:
# Invoker#initialize, which only a literal call reaches, keeps the classes
# its callers hand it, and the Callback it stores is what @subject.new builds
class Base
  def initialize(name, options = {})
    @options = options
  end
  def column? = @options.key?(:column)
end
class Callback
  def initialize(record, *rest) = @record = record
end
class Invoker
  def initialize(subject, record, args)
    @subject = subject
    @record = record
    @args = args
  end
  def run = @subject.new(@record, *@args)
end
class Model
  def publish!(*args) = Invoker.new(Callback, self, args).run
  def fire(name) = __send__(name)
end
b = Base.new(:default, { column: :state })
Model.new.publish!(1, 2)
Model.new.fire((ARGV[0] || "publish!").to_sym)
p b.column?
