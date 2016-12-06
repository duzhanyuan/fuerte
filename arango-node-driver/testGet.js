var arangodb = require('bindings')('node-arangodb');
var vpack = require(__dirname + "/node_modules/node-velocypack/build/Debug/node-velocypack.node")

// create server and connection
var serverUrl = "http://127.0.0.1:8529"
serverUrl = "vstream://127.0.0.1:8529"
var server = new arangodb.Server(serverUrl);
var conn = server.makeConnection();

// create post request
conUrl = new arangodb.ConnectionUrl()
conUrl.setServerUrl(serverUrl)
conUrl.setDbName("testdb")
conUrl.setTailUrl("/_api/document/testcol/123456")

conn.reset() // must be called first
conn.setHeaderOpts();
conn.setUrl(conUrl)
//request_data = vpack.encode({"data" : "banana"})
//conn.setPostField(request_data)
conn.setGetReq();
conn.setBuffer(); // must be called last

//fire request
sink = conn.SetAsynchronous(true);
new Promise(function (resolve, reject) {
 function run() {
   try {
     conn.Run();
     if (conn.IsRunning()) {
       setImmediate(run);
     } else {
       resolve();
     }
   } catch (e) {
     reject(e);
   }
 }
 run();
})
.then(function () {
  let vpResult = conn.Result();
  console.log('server response', vpResult)
  jsResult = vpack.decode(vpResult)
  console.log("result :", jsResult);
})
.catch(function (e) {
  console.error(e.stack);
});