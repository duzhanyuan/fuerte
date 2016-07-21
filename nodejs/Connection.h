////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author John Bufton
////////////////////////////////////////////////////////////////////////////////
#ifndef FUERTE_NODE_CONNECTION_H
#define FUERTE_NODE_CONNECTION_H

#include <fuerte/Connection.h>
#include <nan.h>

namespace arangodb {

namespace dbnodejs {

class Connection : public Nan::ObjectWrap {
 private:
  typedef arangodb::dbinterface::Connection PType;

 public:
  typedef PType::SPtr Ptr;
  Connection();
  Connection(const Ptr inp);
  const Ptr libConnection() const;
  static void Run(const Nan::FunctionCallbackInfo<v8::Value>& info);
  static void Address(const Nan::FunctionCallbackInfo<v8::Value>& info);
  static void New(const Nan::FunctionCallbackInfo<v8::Value>& info);
  static void Init(v8::Local<v8::Object> exports);
  static v8::Local<v8::Value> NewInstance(v8::Local<v8::Value> arg);

 private:
  Ptr _pConnection;
  static Nan::Persistent<v8::Function> _constructor;
};

inline const Connection::Ptr Connection::libConnection() const {
  return _pConnection;
}
}
}

#endif  // FUERTE_NODE_CONNECTION_H
