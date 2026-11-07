# A rescued exception boxes under the builtin exception slot. Its class's
# modules, and case/when on its class, must still match.
module Tagged; def tag = "T:#{message}"; end
module Other; end
class AppError < StandardError; include Tagged; end
class SubError < AppError; end
class PlainError < StandardError; end
class NotAnError; include Tagged; end

def grab
  yield
rescue => e
  e
end

list = [
  grab { Integer("zz") },
  grab { raise SubError, "sub" },
  grab { raise PlainError, "plain" },
  grab { raise AppError, "app" },
  grab { [].fetch(3) },
]

list.each do |e|
  p [e.class, e.is_a?(Tagged), e.kind_of?(Other), Tagged === e,
     e.class.include?(Tagged), e.class.ancestors.include?(Tagged)]
  case e
  when Tagged then puts "tagged #{e.message}"
  else puts "not tagged #{e.class}"
  end
  case e
  when AppError then puts "app family"
  when PlainError then puts "plain"
  when ArgumentError then puts "argument"
  else puts "else #{e.class}"
  end
end

p list.map(&:class)
p list[1].class.ancestors.take(4)
p list[1].class.superclass
p list[1].class == SubError
p list[0].class == ArgumentError
p NotAnError.new.is_a?(Tagged)
p [3, "s", list[1], list[0]].map { |v| v.is_a?(Tagged) }

begin
  raise SubError, "r"
rescue Tagged => e
  p [:rescued_by_module, e.class]
end
