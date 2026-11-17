# Reduced from an emitted Rails app: a late re-bind of parameters widens
# OpenSSL::Digest's @name (its uncalled `self.file` hands `new` an unknown
# name), and MD5, whose copy of initialize wrote `super("MD5")`'s String,
# keeps that slot too. Nothing here runs; the program must compile.
module Rack
  class Request
    def POST
      form = ActionDispatch::Http::Multipart.parse(body, content_type.to_s)
      form.file_parts.each { |part| CgiIo.assign_form_pair(hash, part.name, part.file) }
      @env["rack.request.form_input"] = input
    end
    def params
      self.POST
    end
  end
end
module ActiveStorage
  class Preview
    def process
      Previewer.poster(b)
    end
    def variant
      process
    end
  end
  class Filename
    def to_s
    end
  end
end
module ActionDispatch
  class Request
    def self.for(env, params = {})
      params.each { |k, v| r.params[k.to_s] = v }
    end
  end
end
module OpenSSL
  class Digest
    def self.file(name) = new(name)
    def initialize(name, data = nil)
      @name = name.to_s.upcase
    end
    class MD5 < Digest
      def initialize(data = nil) = super("MD5")
    end
  end
end
module ActionController
  class CookieJar
    def initialize(inbound = {})
      inbound.each { |k, v| @inbound[k.to_s] = v }
    end
    def []=(key, value)
      raw_set(key, value)
    end
    def raw_set(key, value)
      @deleted.delete(key.to_s)
      @out[key.to_s] = value.to_s
    end
    def pending
      @out
    end
  end
  class Base
    def cookies
      @cookies = ActionController::CookieJar.new
    end
  end
end
module ActionDispatch
  module Http
    module Multipart
      class FilePart
      end
      class Form
        attr_reader :file_parts
        def add_file(name, file)
          @file_parts.push(FilePart.new)
        end
      end
      def self.parse(body, content_type)
        form = Form.new
      end
    end
  end
end
module ActiveStorage
  module Routes
    def self.instantiate_controller(sym)
      ActiveStorage::Representations::RedirectController.new
    end
  end
  module Representations
    class RedirectController < ActionController::Base
      def process_action(action_name)
        show
      end
      def show
        ActiveStorage::Preview.new(blob, variation).variant
      end
    end
  end
end
module ActiveStorage
  class Previewer
    def self.poster_time_limit
      ActiveStorage.previewers.each do |previewer|
        return previewer.const_get(:TIME_LIMIT)
      end
    end
    def self.poster(path)
      poster_time_limit
    end
  end
end
module RequestDispatch
  def delete(path, params: {}, headers: {}, as: nil)
    dispatch_request("DELETE", path, params, xhr_headers(headers.merge(env), xhr), as)
  end
  def dispatch_request(method, path, params, headers = {}, as = nil)
    answered = GemMiddleware.around(rack_env) do |env|
      controller = ActiveStorage::Routes.instantiate_controller(matched.controller)
      run_controller(controller, matched.action, params, headers, as)
    end
  end
  def run_controller(controller, action, params, headers, as)
    controller.request = ActionDispatch::Request.for(env, merged)
    controller.process_action(action)
    @__response = ActionResponse.new(
      cookies:  ActionController::CookieJar.new(controller.cookies.pending),
    )
  end
end
