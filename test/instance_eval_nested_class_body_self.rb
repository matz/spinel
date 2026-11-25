# spinel: share
class EvalReceiver
  def mark = :receiver
end

class EvalScope
  LABEL = :outer_constant
  @@label = :outer_class_variable

  EvalReceiver.new.instance_eval do
    @label = :receiver_ivar

    class Nested
      def self.mark = :nested
      LABEL = :nested_constant
      @@label = :nested_class_variable
      @label = :nested_ivar
      VALUE = [mark, LABEL, @@label, @label]
    end

    p [mark, LABEL, @@label, @label]
  end

  p Nested::VALUE
end
