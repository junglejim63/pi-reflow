var plotColors = {pv: "#00CC00", out: "#0000CC", sp: "#222222", };

var urlHost = window.location.host;
urlHost = urlHost.toLowerCase().replace('localhost','127.0.0.1');
var socket = io(urlHost); //set this to the ip address of your node.js server
socket.on('news', function (data) {
    console.log(data);
    socket.emit('my other event', { my: 'data' });
});
var highChartRun;
var highChartTune;
var highChartHistory;
Highcharts.setOptions({global: {useUTC: true, timezoneOffset: (new Date()).getTimezoneOffset()}});

//*************************************************
// angular controller
//*************************************************
var app = angular.module("myApp", []);

app.controller("AngularController", function($scope, $http) {
   //for debugging access to $scope:
   window.MYSCOPE = $scope;
   //create charts on startup
   highChartRun = createHighChartsRun();
   highChartTune = createHighChartsTune();
   highChartHistory = createHighChartsHistory();

   //create objects to hold socket.io emitted values
   $scope.run = { title: '', startMS: 1430875166650, endMS: 1430875353800,  TAL: 0, cycleName: '',  status: '',
        profile: [ ], pvHist: [ ], outHist: [ ]};
   $scope.sequences = {};
   $scope.sequence = {};
   $scope.cmdResponse = {};
   $scope.controller = {};
   $scope.tc1 = {};
   $scope.status = {state: "Idle", stateEndMS: 0, stateID: -1};
   $scope.archiveDirList = [];
   $scope.archiveFile = "";
   $scope.archiveRun = { title: '', startMS: 1430875166650, endMS: 1430875353800,  TAL: 0, cycleName: '',  status: '',
        profile: [ ], pvHist: [ ], outHist: [ ]};
   
   //Other assorted scope variables
   $scope.i = {sp: null, out: null, Kp: null, Ti: null, Td: null};
   $scope.nextRunName = "";
   $scope.timeRemaining = "";
   $scope.tunePtCountMax = 2*60*5; //about 5 mins of data at 1/2 sec intervals
   $scope.runChartUpdate = {cnt: 0, freq: 5};
   $scope.tuneChartUpdate = {cnt: 0, freq: 5};
   $scope.activeTab = 'cycle';
   $('#ul-tabs').on('shown.bs.tab', function (e) { //add event listener to bootstrap tab change to record current active tab
     $scope.activeTab = ($("#ul-tabs").children(".active")[0].id).replace("li-tab-", "");
   });
   
   setInterval(function () {
      var now = new Date();
      if ($scope.status.state === "Idle") {
         $scope.timeRemaining = " " + now.toTimeString().substr(0,8);
      } else {
         if ($scope.status.stateEndMS != null) {
            $scope.timeRemaining = " " + formatMS(Math.max($scope.status.stateEndMS-now.getTime(), 0));
         } else {
            $scope.timeRemaining = "";
         }
      }
      $scope.$apply();
   }, 1000)
   
   //event handlers for received objects
   socket.on('Run', function(run) {
      $scope.run = run;
      //update chart
      if (typeof $scope.run.pvHist !== "undefined") highChartRun.get('seriesPV').setData(hist2data($scope.run.pvHist));
      if (typeof $scope.run.profile !== "undefined") highChartRun.get('seriesSP').setData(hist2data($scope.run.profile));
      if (typeof $scope.run.outHist !== "undefined") highChartRun.get('seriesOut').setData(hist2data($scope.run.outHist));
      console.log('Run received');
      $scope.$apply();
      $scope.nextRunName = $scope.run.title;
   });
   socket.on('Sequences', function(sequences) {
      $scope.sequences = sequences;
      console.log('Sequences received');
      $scope.$apply();
   });
   socket.on('Sequence', function(sequence) {
      $scope.sequence = sequence;
      console.log('Sequence received');
      //$scope.$apply();
   });
   socket.on('CommandUpdate', function(cmdResponse) {
      $scope.cmdResponse = cmdResponse;
   });
   socket.on('ControllerUpdate', function(controller) {
      $scope.controller = controller;
      if ($scope.status.state !== "Idle") {
         highChartRun.get('seriesPV').addPoint([$scope.controller.updateTimeMS, $scope.controller.pv], false);
         highChartRun.get('seriesOut').addPoint([$scope.controller.updateTimeMS, $scope.controller.out], false);
         $scope.runChartUpdate.cnt = (++$scope.runChartUpdate.cnt) % $scope.runChartUpdate.freq;
         if ($scope.runChartUpdate.cnt === 0 && $scope.activeTab === 'cycle') highChartRun.redraw(); //reduce redraw frequency
      }
      var ptCountMax = (highChartTune.get('seriesPV').data.length > $scope.tunePtCountMax);
      highChartTune.get('seriesPV').addPoint([$scope.controller.updateTimeMS, $scope.controller.pv], false, ptCountMax);
      highChartTune.get('seriesSP').addPoint([$scope.controller.updateTimeMS, $scope.controller.sp], false, ptCountMax);
      highChartTune.get('seriesOut').addPoint([$scope.controller.updateTimeMS, $scope.controller.out], false, ptCountMax);
      $scope.tuneChartUpdate.cnt = (++$scope.tuneChartUpdate.cnt) % $scope.tuneChartUpdate.freq;
      if ($scope.tuneChartUpdate.cnt === 0 && $scope.activeTab === 'tuning') highChartTune.redraw(); //reduce redraw frequency
      $scope.updateInputFields();
      $scope.$apply();
   });
   socket.on('TC1Update', function(tc1) {
      $scope.tc1 = tc1;
      $scope.$apply();
   });
   socket.on('Status', function(status) {
      $scope.status = status;
      $scope.$apply();
   });
   socket.on('RunUpdate', function(run) { //run doesn't contain all properties
      for (var key in run) {
         if (run.hasOwnProperty(key)) {
            $scope.run[key] = run[key];
         }
      }
      $scope.$apply();
   });
   socket.on('ArchiveDirList', function(fileList) {
      var files = [], match;
      for (var i= 0; i < fileList.length; i++) {
         match = fileList[i].match(/([\w-]+)\.json$/);
         if (match) files.push(match[1]);
      }
      files.sort(); //sort in date descending order
      files.reverse();
      $scope.archiveDirList = files;
      $scope.$apply();
   });
   socket.on('ArchiveFile', function(archive) {
      if (archive.filename == ($scope.archiveFile + ".json")) {
         $scope.archiveRun.title = archive.run.title || "";
         $scope.archiveRun.startMS = archive.run.startMS || 0;
         $scope.archiveRun.endMS = archive.run.endMS || 0;
         $scope.archiveRun.TAL = archive.run.TAL || 0;
         $scope.archiveRun.cycleName = archive.run.cycleName || "";
         $scope.archiveRun.status = archive.run.status || "";
         $scope.archiveRun.profile = archive.run.profile || [];
         $scope.archiveRun.pvHist = archive.run.pvHist || [];
         $scope.archiveRun.outHist = archive.run.outHist || [];
         $scope.$apply();
         //update chart
         if (typeof $scope.archiveRun.pvHist !== "undefined") 
            highChartHistory.get('seriesPV').setData(hist2data($scope.archiveRun.pvHist), false);
         if (typeof $scope.archiveRun.outHist !== "undefined") 
            highChartHistory.get('seriesOut').setData(hist2data($scope.archiveRun.outHist), false);
         if (typeof $scope.archiveRun.profile !== "undefined") 
            highChartHistory.get('seriesSP').setData(hist2data($scope.archiveRun.profile), true); //redraw after last series
      }
   });   
   
   //Functions
   $scope.saveTuning = function() {
      socket.emit('SaveTuning');
   };
   $scope.loadTuning = function() {
      socket.emit('LoadTuning');
   };
   $scope.setSequence = function(cycle) {
      socket.emit('SetSequence', cycle);
   };
   $scope.seqStart = function() {
      socket.emit('SeqStart', $scope.nextRunName);
   };
   $scope.seqStop = function() {
      socket.emit('SeqStop');
   };
   //updates the input object with remote values unless they are being edited (have focus)
   $scope.updateInputFields = function() {
      var focus = $(document.activeElement)[0].id;
      for (key in $scope.i) {
         if ($scope.i.hasOwnProperty(key)) {
            if (focus != "input-"+key) {
               $scope.i[key] = $scope.controller[key];
            }
         }
      }
   };
   //send changed value to controller
   $scope.sendChange = function(key, val) {
      var o = {};
      o[key] = val;
      socket.emit('SetControllerValue', o);
   };
   //fetch requested archive file
   $scope.readArchiveFile = function() {
      if (typeof $scope.archiveFile !== 'string' || $scope.archiveFile.length < 10) return;
      var filename = $scope.archiveFile + ".json";
      socket.emit('ReadArchiveFile', filename);
   }
   $scope.formatMSLocal = function(ms) {
      return (new Date(ms)).toLocaleString();
   }
         
   //send initial request to populate objects
   socket.emit('Sequences');
   socket.emit('Sequence');
   socket.emit('Run');
});

//*************************************************
// Convert history array to data series
//*************************************************
function hist2data(hist) {
   var data = [];
   for (var i = 0; i < hist.length; i++) {
      if (hist[i] !== null) {
         data.push([hist[i].timestamp, hist[i].value]);
      }
   }
   return data;
}

//*************************************************
// Create the Highcharts chart for the main run tab
//*************************************************
function createHighChartsRun() {
   $('#highchartRun').highcharts({
      chart: {zoomType: 'xy', alignTicks: false, type: 'line'},
      credits: {enabled: false},
      exporting: {enabled: false},
      legend: {enabled: false},
      title: {text: null, style: {fontSize: "12px", color: "black"}},
      tooltip: {shared: true},
      plotOptions: {line: {animation: false}}, 
      xAxis: [{type: "datetime", crosshair: true, labels:{style: {fontSize: "8px"}}}],
      yAxis: [{min:0.0, max: 250, endOnTick: false, startOnTick: false, lineWidth: 0.75, title: {text: null, style: {color: plotColors.pv}},
                labels:{format: "{value}°C", style: {fontSize: "8px", color: plotColors.pv}}},
              {min:-1, max: 101, endOnTick: false, startOnTick: false, lineWidth: 0.75, opposite: true, title: {text: null}, 
                labels:{format: "{value}%", style: {fontSize: "8px", color: plotColors.out}}}],
      series: [
         {id: 'seriesPV', name: 'Temperature', lineWidth: 0.75, color: plotColors.pv, marker: {enabled: false}, yAxis: 0},
         {id: 'seriesSP', name: 'Setpoint', lineWidth: 0.75, color: plotColors.sp, marker: {enabled: false}, yAxis: 0},
         {id: 'seriesOut', name: 'Output', lineWidth: 0.75, color: plotColors.out, marker: {enabled: false}, yAxis: 1}
               ]
   });
   return $('#highchartRun').highcharts();
}

//*************************************************
// Create the Highcharts chart for the tune tab
//*************************************************
function createHighChartsTune() {
   $('#highchartTune').highcharts({
      chart: {zoomType: 'xy', alignTicks: false, type: 'line'},
      credits: {enabled: false},
      exporting: {enabled: false},
      legend: {enabled: false},
      title: {text: null, style: {fontSize: "12px", color: "black"}},
      tooltip: {shared: true},
      plotOptions: {line: {animation: false}}, 
      xAxis: [{type: "datetime", crosshair: true, labels:{style: {fontSize: "8px"}}}],
      yAxis: [{endOnTick: false, startOnTick: false, lineWidth: 0.75, title: {text: null, style: {color: plotColors.pv}},
                labels:{format: "{value}°C", style: {fontSize: "8px", color: plotColors.pv}}},
              {endOnTick: false, startOnTick: false, lineWidth: 0.75, opposite: true, title: {text: null}, 
                labels:{format: "{value}%", style: {fontSize: "8px", color: plotColors.out}}}],
      series: [
         {id: 'seriesPV', name: 'Temperature', lineWidth: 0.75, color: plotColors.pv, marker: {enabled: false}, yAxis: 0},
         {id: 'seriesSP', name: 'Setpoint', lineWidth: 0.75, color: plotColors.sp, marker: {enabled: false}, yAxis: 0},
         {id: 'seriesOut', name: 'Output', lineWidth: 0.75, color: plotColors.out, marker: {enabled: false}, yAxis: 1}
               ]
   });
   return $('#highchartTune').highcharts();
}

//*************************************************
// Create the Highcharts chart for the history tab
//*************************************************
function createHighChartsHistory() {
   $('#highchartHistory').highcharts({
      chart: {zoomType: 'xy', alignTicks: false, type: 'line'},
      credits: {enabled: false},
      exporting: {enabled: false},
      legend: {enabled: false},
      title: {text: null, style: {fontSize: "12px", color: "black"}},
      tooltip: {shared: true},
      plotOptions: {line: {animation: false}}, 
      xAxis: [{type: "datetime", crosshair: true, labels:{style: {fontSize: "8px"}}}],
      yAxis: [{endOnTick: false, startOnTick: false, lineWidth: 0.75, title: {text: null, style: {color: plotColors.pv}},
                labels:{format: "{value}°C", style: {fontSize: "8px", color: plotColors.pv}}},
              {endOnTick: false, startOnTick: false, lineWidth: 0.75, opposite: true, title: {text: null}, 
                labels:{format: "{value}%", style: {fontSize: "8px", color: plotColors.out}}}],
      series: [
         {id: 'seriesPV', name: 'Temperature', lineWidth: 0.75, color: plotColors.pv, marker: {enabled: false}, yAxis: 0},
         {id: 'seriesSP', name: 'Setpoint', lineWidth: 0.75, color: plotColors.sp, marker: {enabled: false}, yAxis: 0},
         {id: 'seriesOut', name: 'Output', lineWidth: 0.75, color: plotColors.out, marker: {enabled: false}, yAxis: 1}
               ]
   });
   return $('#highchartHistory').highcharts();
}

function formatMS(duration) {
   var day = 24*60*60*1000;
   var hour = 60*60*1000;
   var minute = 60*1000;
   var result = (duration > day) ? Math.floor(duration/day) + " " : ""; //days if necessary
   duration = duration % day;
   result += ('00'+ Math.floor(duration/hour)).substr(-2) + ":"; //hours
   duration = duration % hour;
   result += ('00'+ Math.floor(duration/minute)).substr(-2) + ":"; //minutes
   duration = duration % minute;
   result += ('00'+ Math.floor(duration/1000)).substr(-2);  //seconds
   return result;
}

