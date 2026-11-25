# spinel: share
class EvalReceiver
  def mark(value = :receiver)
    value
  end

  def visit(value = :visited)
    yield value
  end
end

class OtherEvalReceiver
  def mark(value = :other)
    value
  end
end

class EvalScope
  @@label = :class_variable
  LABEL = :constant

  def self.mark(value = :class_body)
    value
  end

  def self.visit(value = :class_visit)
    yield value
  end

  def self.only_on_class(value = :class_only)
    value
  end

  def self.run(&block)
    EvalReceiver.new.instance_eval(&block)
  end

  def self.run_poly(receiver, &block)
    receiver.instance_eval(&block)
  end

  receiver = EvalReceiver.new
  run do
    @tag = :instance_variable
    p @tag
    p @@label
    p LABEL
    p self.class.name
    p mark
    p respond_to?(:mark)
    p respond_to?(:only_on_class)
    begin
      only_on_class(:argument)
    rescue NameError => error
      p error.class
    end
    begin
      only_on_class
    rescue NameError => error
      p error.class
    end
  end
  receiver.instance_eval do
    p mark
    visit { |value| p value }
    OtherEvalReceiver.new.instance_eval { p mark }
    p mark
  end
  [0].each do
    p mark
    visit { |value| p value }
  end

  poly = ARGV.empty? ? receiver : OtherEvalReceiver.new
  run_poly(poly) { p mark }
  poly = ARGV.empty? ? OtherEvalReceiver.new : receiver
  run_poly(poly) { p mark }
end

module EvalModule
  def self.mark(value)
    :module_body
  end

  def self.visit(value)
    yield :module_visit
  end

  EvalReceiver.new.instance_exec(:exec_argument) do |argument|
    p mark(argument)
    visit(argument) { |value| p value }
  end
end

class EvalOnInteger
  def self.abs
    99
  end

  (-2).instance_eval { p abs }
end
