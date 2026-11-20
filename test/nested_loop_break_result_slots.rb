def keyword_loop_result
  i = 0
  while i < 2
    i += 1
    loop { break +"Kernel result" }
    while true
      break +"while result"
    end
    until false
      break +"until result"
    end
    for value in [1]
      break +"for result"
    end
    break +"outer result" if i == 20
  end
end

def value_loop_result
  i = 0
  until i == 2
    i += 1
    loop { break +"ignored" }
    break +"kept" if i == 1
  end
end

p keyword_loop_result
p value_loop_result

word = +"outer"
events = []
selected = while true
  for row in (begin
    break word if events.empty?
    [1]
  ensure
    events << :ensured
    GC.start
  end)
    events << row
  end
  break :tail
end
p selected.equal?(word), events
