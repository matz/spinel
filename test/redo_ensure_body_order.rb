attempt = 0
events = []
word = +"kept"
values = [word].map do |item|
  begin
    begin
      attempt += 1
      events << attempt
      redo if attempt == 1
      item
    ensure
      events << :inner
      GC.start
    end
  ensure
    events << :outer
  end
end
p events, values.first.equal?(word)

def replaces_pending_throw
  word = +"before"
  attempts = 0
  catch(:done) do
    [word].each do |item|
      begin
        begin
          attempts += 1
          throw :done if attempts == 1
          item.replace("retried")
        ensure
          redo if attempts == 1
        end
      ensure
        GC.start
      end
    end
  end
  p [attempts, word]
end

def keeps_pending_throw
  word = +"before"
  attempts = 0
  result = catch(:done) do
    begin
      throw :done, word
    ensure
      [word].each do |item|
        begin
          attempts += 1
          redo if attempts == 1
          item.replace("seen")
        ensure
          GC.start
        end
      end
    end
  end
  p [attempts, word, result.equal?(word)]
end

replaces_pending_throw
keeps_pending_throw

attempts = 0
result = catch(:outer) do
  catch(:inner) do
    [1].each do
      begin
        attempts += 1
        redo if attempts == 1
        throw :inner, +"inner"
      ensure
        if attempts == 1
          begin
            throw :outer, +"discarded"
          ensure
            GC.start
            redo
          end
        end
      end
    end
  end
end
p [attempts, result]

# A transfer inside an ensure can reach the same destination as the
# suspended transfer. If redo cancels it, a break or return still carries
# its original object through any allocations in the ensure.
def keep_break_value(&block)
  attempts = 0
  begin
    block.call(String.new("original break"))
  ensure
    [1].each do |_item|
      begin
        attempts += 1
        redo if attempts == 1
      ensure
        if attempts == 1
          begin
            block.call(String.new("interrupted break"))
          ensure
            GC.start
            redo
          end
        end
      end
    end
  end
end

def keep_return_value
  leave = proc { |value| return value }
  attempts = 0
  begin
    leave.call(String.new("original return"))
  ensure
    [1].each do |_item|
      begin
        attempts += 1
        redo if attempts == 1
      ensure
        if attempts == 1
          begin
            leave.call(String.new("interrupted return"))
          ensure
            GC.start
            redo
          end
        end
      end
    end
  end
end

p keep_break_value { |value| break value }
p keep_return_value
