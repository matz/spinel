# A statement `case` on an Integer that can be nil binds its subject beside
# a nil flag. The next method's `case` on a plain Integer numbers its subject
# temp from the same start, and must not read a flag it never declared.

def nilable_subject(flag)
  n = nil
  n = 1 if flag
  case n
  when nil then puts "nil"
  when 0 then puts "zero"
  else puts "other #{n}"
  end
end

def literal_subject
  case 3
  when 0 then puts "zero"
  when 3 then puts "three"
  end
end

def plain_subject(k)
  case k
  when 0 then puts "zero"
  when nil then puts "nil"
  else puts "plain #{k}"
  end
end

def range_subject(k)
  case k
  when 0..2 then puts "low"
  when 3.. then puts "high"
  end
end

def in_loop(k)
  i = 0
  begin
    case k
    when 0 then puts "loop zero"
    else puts "loop #{k}"
    end
    i += 1
  end while i < 2
end

nilable_subject(true)
nilable_subject(false)
literal_subject
plain_subject(5)
range_subject(1)
range_subject(7)
in_loop(4)

case 4
when 4 then puts "top four"
end
