# Conditional dispatchers keep both method availability and argument counts.
# Arguments run once even when the selected method cannot take them.
$events = []
def argument
  $events << :argument
  1
end

def call_pair(receiver)
  begin
    p receiver.pair(argument)
  rescue => error
    p [error.class, error.respond_to?(:name) ? error.name : nil]
  end
end

class ConditionalPair
end
pair = ConditionalPair.new
call_pair(pair)
p $events.dup
if ARGV.empty?
  class ConditionalPair
    def pair(left, right)
      $events << :body
      [left, right]
    end
  end
end
call_pair(pair)
p $events

class ConditionalZero
  if ARGV.empty?
    def value(&block)
      :body
    end
  end
end
begin
  p ConditionalZero.new.value(1)
rescue => error
  p [error.class, error.respond_to?(:name) ? error.name : nil]
end
