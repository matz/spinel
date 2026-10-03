# A finalizer defined by a class_eval template is still a finalizer: the
# template is a string until it is expanded, so a program that evaluates a
# string keeps the finalizer API wherever the word is spelled.
class Thing; end

class Watcher
  KINDS = %w[first second]
  KINDS.each do |kind|
    class_eval <<~RUBY
      def watch_#{kind}(obj)
        ObjectSpace.define_finalizer(obj, proc { |_id| puts "gone" })
        :#{kind}
      end
    RUBY
  end
end

w = Watcher.new
p w.watch_first(Thing.new)
p w.watch_second(Thing.new)
puts "end"
