# `v !~ /re/` on a boxed receiver (a String, a Symbol, nil, or an object
# without =~, each read from an accessor a poly Hash fills) answers a C truth
# value: the negated match for a String or a Symbol's name, true for nil,
# NoMethodError for the rest. Where the call's slot is boxed, as an `if`
# condition read through sp_poly_truthy is in a large application program's
# mailto URI checks, the answer is boxed as match?'s is; it was handed as an
# int. The shapes here read it as a condition, a value and an argument.
class Matcher
  def =~(re) = nil
end
class MailTo
  HEADER_REGEXP = /\A[a-z]+=/
  attr_reader :to, :headers
  def initialize(opts)
    @to = nil
    @headers = nil
    @to = opts[:to] if opts.key?(:to)
    @headers = opts[:headers] if opts.key?(:headers)
  end
  def check!
    if to !~ /\A(?:[^@,;]+@[^@,;]+(?:\z|[,;]))*\z/
      raise ArgumentError, "unrecognised: #{to}"
    end
    if !headers.nil? && !headers.empty? && headers !~ HEADER_REGEXP
      raise ArgumentError, "bad headers"
    end
    true
  end
  def odd? = (to !~ /@/)
  def report = [to !~ /@/, headers !~ /=/, headers !~ HEADER_REGEXP]
end
opts = ARGV.empty? ? { to: "a@b.c", headers: "subject=hi" } : { to: Matcher.new, headers: :sym }
m = MailTo.new(opts)
p m.check!, m.odd?, m.report
begin
  MailTo.new({ to: "x,," }).check!
rescue ArgumentError => e
  puts e.message
end
begin
  MailTo.new({}).check!
rescue ArgumentError => e
  puts e.message.inspect
end
p MailTo.new({}).odd?, MailTo.new({ to: :sym }).report
begin
  MailTo.new({ to: 5 }).odd?
rescue NoMethodError => e
  puts e.message
end
