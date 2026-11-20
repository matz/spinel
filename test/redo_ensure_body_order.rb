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
