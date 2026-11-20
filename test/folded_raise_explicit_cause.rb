# frozen_string_literal: true

def boolean_answer(key, message)
  return false if key == 0
  (key.is_a?(String) ? true : raise(KeyError, message, cause: nil))
end

def integer_answer(key, message)
  return 6 if key == 0
  (key.is_a?(String) ? 7 : raise(KeyError, message, cause: nil))
end

message = +"message"
p boolean_answer(0, message)
begin
  boolean_answer(1, message)
rescue KeyError => error
  p [error.message.equal?(message), error.cause]
end
p integer_answer(0, message)
begin
  integer_answer(1, message)
rescue KeyError => error
  p [error.message.equal?(message), error.cause]
end
