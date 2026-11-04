# A declaration told to self's singleton class inside a class_eval block
# whose statements run in a class body: a module's
# `def self.included(base); base.class_eval do ... end; end` hook (the shape
# an ActiveSupport::Concern `included do ... end` block is spelled in when
# written out), or a bare class_eval statement of a body. The hook's
# `singleton_class.alias_method :new, :old` aliases the including class's
# class method, as its statement form in a class body does; it was refused
# as Object#singleton_class.
module Redirecting
  def self.included(base); base.class_eval do
    singleton_class.alias_method :allowed_hosts, :_allowed_hosts
    singleton_class.attr_accessor :redirect_mode
  end; end
end

class Controller
  def self._allowed_hosts = ["example.com"]
  include Redirecting
end

class Admin
  def self._allowed_hosts = ["admin.example.com"]
  include Redirecting
end

class Plain
  def self.version = 2
  class_eval do
    singleton_class.alias_method :revision, :version
  end
end

Controller.redirect_mode = :log
p Controller.allowed_hosts, Admin.allowed_hosts
p Controller.redirect_mode, Admin.redirect_mode
p Plain.revision
