# frozen_string_literal: true

answer = { 1 => true, :other => false }.transform_values do |first|
  begin
    next "a" * 3 if first
    "b" * 4
  ensure
    GC.start
    +"ensure allocates"
  end
end
p answer
