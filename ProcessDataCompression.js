var util = require("util");
var events = require("events");

//*******************************************
// Process Data Compression
//*******************************************
function ProcessDataCompression(config) {
   events.EventEmitter.call(this);
   var defConfig = {type: "BOXCARBACKSLOPE", tol: 1.0, maxMS : 1000*60*60*4};
   config = config || defConfig;
   this.type = config.type || defConfig.type;
   this.tolerance = config.tol || defConfig.tol;
   this.maxMS = config.maxMS || defConfig.maxMS;
   this.valueOnError =  -2147483648;
   //-2147483648 is a somewhat arbitrary number extremely unlikely to be a valid input - 0x8000_0000 for 32 bit int
   //keeping an invalid value in the 'number' realm greatly simplifies the compression algorithm, and
   //it can be switched to null or some other value as it is logged.
   this.last = {value: null, timestamp: 0};
   this.lastSaved = {value: null, timestamp: null};
   this.boxcar = {slope: null, boxcarFail: false, slopeFail: false};
   this.swingDoor = {slopeMax: null, slopeMin: null};
   this.scanStatus = "OFFSCAN";
}
util.inherits(ProcessDataCompression, events.EventEmitter);

//*******************************************
// logPoint method
//*******************************************
ProcessDataCompression.prototype.logPoint = function(dataPoint, reason) { //dataPoint is object {value: 0.0, timestamp: nnnnnnnnnn}
   var val = (dataPoint.value === this.valueOnError) ? null : dataPoint.value;
   var ret = {value: val, timestamp: dataPoint.timestamp, tolerance: this.tolerance, reason: reason};
   this.lastSaved = dataPoint;
   this.boxcar = {slope: null, boxcarFail: false, slopeFail: false};
   this.swingDoor = {slopeMax: null, slopeMin: null};
   return ret;
};

//*******************************************
// Start method
//*******************************************
ProcessDataCompression.prototype.start = function(dataPoint) { //dataPoint is object {value: 0.0, timestamp: nnnnnnnnnn}
   var ret = [];
   if (this.scanStatus !== "OFFSCAN" || !this.dpValid(dataPoint)) {return ret;}
   this.scanStatus = "ONSCAN";
   dataPoint.value = this.getNumberValue(dataPoint.value);  //make sure value is a number, valueOnError if not
   // if value was not null, log a 1ms previous null value followed by this value.
   if (dataPoint.value !== this.valueOnError) {
      ret = [{value: null, timestamp: dataPoint.timestamp-1, 
                    tolerance: this.tolerance, reason: "STARTUP"}];  //create null datapoint 1 ms before first scan point
   }
   ret.push(this.logPoint(dataPoint, "STARTUP"));  //and log the current point
   this.last = dataPoint;
   return ret;
};

//*******************************************
// scan method
//*******************************************
ProcessDataCompression.prototype.scan = function(dataPoint) { //dataPoint is object {value: 0.0, timestamp: nnnnnnnnnn}
   var ret = []; 
   var reason = "COMPRESSION";
   if (this.scanStatus !== "ONSCAN" || !this.dpValid(dataPoint)) {return ret;}
   dataPoint.value = this.getNumberValue(dataPoint.value);  //make sure value is a number, valueOnError if not
   var log = false;
   //test to see if max time has elapsed
   log = dataPoint.timestamp > (this.lastSaved.timestamp + this.maxMS);
   //test value to see if compression violation has occurred
   if (log) {
      reason = "EXPIRED";
   } else {
      log = this.logCheck(dataPoint);
   }
   if (log) {
      ret.push(this.logPoint(this.last, reason));  // log the previous point
      this.logCheck(dataPoint); //check current point against new last saved point.  This will never log a point, but will set slopes and check for tolerance failures
   }
   this.last = dataPoint;
   return ret;
};

//*******************************************
// Stop method
//*******************************************
ProcessDataCompression.prototype.stop = function(dataPoint) { //dataPoint is object {value: 0.0, timestamp: nnnnnnnnnn}
   //perform initial scan on new point just as if it were another point coming in
   var ret = this.scan(dataPoint);
   //add the latest point if it is valid - if datapoint is valid this.last would now be the same as dataPoint (in .scan)
   if (this.scanStatus !== "ONSCAN" || dataPoint.timestamp !== this.last.timestamp) {return ret;}
   ret.push(this.logPoint(this.last, "SHUTDOWN"));  //and log the current point
   //if last point value was not null, also add a null value 1 ms later
   if (this.last.value !== this.valueOnError) {
      ret.push({value: null, timestamp: this.last.timestamp+1, 
                    tolerance: this.tolerance, reason: "SHUTDOWN"});  //create null datapoint 1 ms after last scan point
   }
   this.scanStatus = "OFFSCAN";
   return ret;
};

//*******************************************
// logCheck method runs logCheck based on the compression method
//*******************************************
ProcessDataCompression.prototype.logCheck = function(dataPoint) { //dataPoint is object {value: 0.0, timestamp: nnnnnnnnnn}
   switch (this.type) {
      case "BOXCARBACKSLOPE":
         return this.logCheckBoxcar(dataPoint);
         break;
      case "SWINGINGDOOR":
         return this.logCheckSwingDoor(dataPoint);
         break;
      default:
         break;
   }
   return false;
};

//*******************************************
// logCheckBoxcar method - determines if point needs to be logged
//*******************************************
ProcessDataCompression.prototype.logCheckBoxcar = function(dataPoint) {
   var elapsedMS = dataPoint.timestamp - this.lastSaved.timestamp;
   //check boxcar
   if (!this.boxcar.boxcarFail) {
      if (Math.abs(dataPoint.value - this.lastSaved.value) >= this.tolerance) {
         this.boxcar.boxcarFail = true;
      }
   }
   //calculate slope
   if (this.boxcar.slope === null) {
      this.boxcar.slope = (dataPoint.value - this.lastSaved.value)/elapsedMS;
   } else { //check back slope
      if (!this.boxcar.slopeFail) {
         var projectedVal = this.lastSaved.value + this.boxcar.slope * elapsedMS;
         if (Math.abs(dataPoint.value - projectedVal) >= this.tolerance) {
            this.boxcar.slopeFail = true;
         }
      }
   }
   //determine if both backslope and boxcar tests have failed
   if (this.boxcar.boxcarFail && this.boxcar.slopeFail) {
       return true;
   }
   
   return false;
};

//*******************************************
// logCheckSwingDoor method - determines if point needs to be logged
//*******************************************
ProcessDataCompression.prototype.logCheckSwingDoor = function(dataPoint) {
   var elapsedMS = dataPoint.timestamp - this.lastSaved.timestamp;
   var sMax = (dataPoint.value + this.tolerance - this.lastSaved.value)/elapsedMS;
   var sMin = (dataPoint.value - this.tolerance - this.lastSaved.value)/elapsedMS;
   if (this.swingDoor.slopeMax === null) { //set initial slopes if none set
      this.swingDoor.slopeMax = sMax;
      this.swingDoor.slopeMin = sMin;
      return false;
   }
   //check if current point's slope exceeds slope max or min
   var s = (dataPoint.value - this.lastSaved.value)/elapsedMS;
   if (s > this.swingDoor.slopeMax || s < this.swingDoor.slopeMin) {return true;}
   //adjust slopeMax and or slopeMin based on new point
   this.swingDoor.slopeMax = Math.min(sMax, this.swingDoor.slopeMax);
   this.swingDoor.slopeMin = Math.max(sMin, this.swingDoor.slopeMin);
   return false;
};

//*******************************************
// getNumberValue method - converts numbers as string or nulls to floats
//*******************************************
ProcessDataCompression.prototype.getNumberValue = function(value) {
   if (typeof value !== 'undefined') {
      if (isNumber(value)) {
         return parseFloat(value);
      }
   }
   return this.valueOnError;
};

//*******************************************
// dpValid method - //makes sure provided data point is valid
//*******************************************
ProcessDataCompression.prototype.dpValid = function(dp) {
   if (typeof dp !== "object" ||                       //not an object
       typeof dp.timestamp !== "number" ||        //timestamp is not a number
       dp.timestamp <= this.last.timestamp ||   //timestamp is before or same as last timestamp (works correctly if last = null)
       typeof dp.value == "undefined") {        //value does not exist
      return false;
   } else {
      return true;
   }
};

//*******************************************
// Utility functions
//*******************************************
function isNumber(n) {  //from http://stackoverflow.com/questions/18082/validate-numbers-in-javascript-isnumeric
  return !isNaN(parseFloat(n)) && isFinite(n);
}

module.exports.ProcessDataCompression = ProcessDataCompression;
