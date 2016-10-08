var Immutable = require('immutable');
var React = require('react')
var ReactDOM = require('react-dom');
var _ = require('lodash');
var $ = require('jquery');
var update = require('react-addons-update');
var Video = require('react-html5video/dist/ReactHtml5Video').default;

$.whenall = function(arr) {
    return $.when.apply($, arr).pipe(function() {
        return Array.prototype.slice.call(arguments);
    });
};

require("../css/index.scss");

var item_bbox = {
  frame: 100,
  time: 36,
  data: {
    bboxes: [
      {
        x: 100,
        y: 125,
        width: 50,
        height: 20,
      },
      {
        x: 200,
        y: 75,
        width: 28,
        height: 60,
      },
    ],
  },
};

var classificationGraphics = {
  setup: function(mainPanel) {
  },
  plotValues: function(sampledData) {
    var confidenceData = [];
    for (var i = 0; i < sampledData.length; ++i) {
      var item = sampledData[i];
      // var norm = 0;
      // for (var j = 0; j < item.data.length; ++j) {
      //     norm += item.data[j] * item.data[j];
      //     if (item.data[j] > max) {
      //         max = item.data[j];
      //     }
      // }
      // var max = 0.0;
      // for (var j = 0; j < item.data.length; ++j) {
      //     if (item.data[j] / norm > max) {
      //         max = item.data[j] / norm;
      //     }
      // }
      // confusionData.append({
      //     frame: i,
      //     confusion: max
      // });
      confidenceData.push({
        frame: item.frame,
        value: item.data.confidence,
      });
    }
    return confidenceData;
  },
  show: function() {
    var classIndicator = $("#class-indicator");
    classIndicator.show();
  },
  draw: function(videoMetadata, item) {
    var classIndicator = $("#class-indicator");
    classIndicator.text(item.data.confidence + "\n" +
                        labelNames[item.data["class"]]);
  },
  hide: function() {
    var classIndicator = $("#class-indicator");
    classIndicator.hide();
  },
};

var detectionGraphics = {
  setup: function(container, videoElement) {
    var container = $(container);
    var videoElement = $(videoElement);
    detectionGraphics.svgContainer = d3.select(container.get(0))
                                       .append("svg")
                                       .classed("bbox-container", true)
                                       .attr("width", videoElement.width())
                                       .attr("height", videoElement.height())
                                       .append("g");
  },
  teardown: function(container) {
    var container = $(container);
    d3.select(container.get(0)).selectAll("svg").remove();
  },
  show: function() {
  },
  draw: function(videoElement, videoMetadata, frame, boxes, c, color) {
    var videoElement = $(videoElement);
    var bboxes = detectionGraphics.svgContainer.selectAll(".bbox-" + c)
                                  .data(boxes, function (d, i) {
                                    var id = frame + ':' + i;
                                    return id;
                                  });
    var w = videoMetadata.width;
    var h = videoMetadata.height;
    var viewWidth = videoElement.width();
    var viewHeight = videoElement.height();
    bboxes.enter()
          .append("rect")
          .attr("class", "bbox-" + c)
          .attr("x", function(d) {
            return (d.x - d.width / 2) / w * viewWidth;
          })
          .attr("y", function(d) {
            return (d.y - d.height / 2) / h * viewHeight;
          })
          .attr("width", function(d) {
            return d.width / w * viewWidth;
          })
          .attr("height", function(d) {
            return d.height / h * viewHeight;
          })
          .style("stroke", color);
    bboxes
      .attr("x", function(d) {
        return (d.x - d.width / 2) / w * viewWidth;
      })
      .attr("y", function(d) {
        return (d.y - d.height / 2) / h * viewHeight;
      })
      .attr("width", function(d) {
        return d.width / w * viewWidth;
      })
      .attr("height", function(d) {
        return d.height / h * viewHeight;
      })
      .style("stroke", color);
    bboxes.exit()
          .remove();
  },
  hide: function() {
    // detectionGraphics.svgContainer.selectAll("rect.bbox")
    //     .data([])
    //     .exit()
    //     .remove();
  },
  //
};

var graphicsOptions = {
  "classification": classificationGraphics,
  "detection": detectionGraphics,
};

function draw(v,c) {
  c.drawImage(v,0,0,c.canvas.width,c.canvas.height);
}

function setupViewer(container, mainPanel, jobMetadata, videoMetadata) {
  var viewer = $('<div/>', {'class': 'video-viewer'});
  container.append(viewer)

  var video = $('<video/>', {'width': viewWidth,
                             'height': viewHeight,
                             'class': 'video-viewer',
                             'controls': 'on'})
    .hide();
  viewer.append(video);

  /*var videoSource = $('<source/>', {'src': videoMetadata.mediaPath,
     'type': 'video/mp4'});
     video.append(videoSource);*/

  var timeline = $('<div/>', {'class': 'video-timeline'});
  viewer.append(timeline);
  var timelineThumbnails = $('<div/>', {'class': 'video-thumbnails'});
  timeline.append(timelineThumbnails);

  var htmlVideo = video[0];

  var totalDuration = 0;
  $(htmlVideo).on("loadedmetadata", function() {
    totalDuration = this.duration;
    htmlVideo.currentTime = 1;
  });

  var thumbnailsContainer = timelineThumbnails;
  var thumbnailWidth = 100;
  var thumbnailHeight = 56.26;
  var totalThumbnails =
    Math.floor(thumbnailsContainer.width() / thumbnailWidth);

  //var currentThumbnail = 0;
  var currentThumbnail = totalThumbnails;
  for (var i = 0; i < totalThumbnails; ++i) {
    var thumbnailCanvas = $('<canvas/>',{'class':'thumbnail'})
      .width(thumbnailWidth)
      .height(thumbnailHeight);
    thumbnailsContainer.append(thumbnailCanvas);
  }

  $(htmlVideo).on("seeked", function() {
    if (currentThumbnail < totalThumbnails) {
      var thumbnailContext = thumbnailsContainer
        .children()[currentThumbnail]
        .getContext("2d");
      draw(this, thumbnailContext);

      currentThumbnail += 1;
      this.currentTime =
        (totalDuration / (totalThumbnails)) * currentThumbnail;
    }
  });

  setupTimeline(timeline, mainPanel, htmlVideo, jobMetadata, videoMetadata);
}

function setupTimeline(container,
                       mainPanel,
                       video,
                       jobMetadata,
                       videoMetadata)
{
  var requestRadius = 1;
  var stride = 1;

  var mainVideo = mainPanel.children("#main-viewer")[0];
  var mainVideoSource = $(mainVideo).children("source");
  var frameIndicator = mainPanel.children("#frame-indicator");
  var classIndicator = mainPanel.children("#class-indicator");

  $(mainVideo).attr('width', viewWidth);
  $(mainVideo).attr('height', viewHeight);

  var width = container.width();
  var tickWidth = 100;
  var tickHeight = 80;
  var ticks = Math.floor(width / tickWidth);

  var hoveredFrame = -1;
  var selectedFrame = -1;
}

function setupTimelinePlot(axis,
                           video,
                           jobMetadata,
                           videoMetadata,
                           plotWidth,
                           plotHeight,
                           predictionData)
{
  var plotCanvas = $('<canvas/>', {'class': 'timeline-plot',
                                   'width': plotWidth,
                                   'height': plotHeight})
    .css('width', plotWidth)
    .css('height', plotHeight);
  axis.append(plotCanvas)

  plotCanvas[0].width = plotWidth;
  plotCanvas[0].height = plotHeight;
  var context = plotCanvas[0].getContext("2d");

  var margin = {top: 0, right: 0, bottom: 0, left: 0},
      canvasWidth = plotWidth - margin.left - margin.right,
      canvasHeight = plotHeight - margin.top - margin.bottom;

  var x = d3.scaleLinear()
            .range([0, canvasWidth]);

  var y = d3.scaleLinear()
            .range([canvasHeight, 0]);

  var line = d3.line()
               .x(function(d) { return x(d.frame); })
               .y(function(d) { return y(d.value); })
               .curve(d3.curveStep)
               .context(context);

  context.translate(margin.left, margin.top);

  var lineData = jobMetadata.graphics.plotValues(predictionData);

  //x.domain(d3.extent(confusionData, function(d) { return d.frame; }));
  x.domain([0, videoMetadata.frames]);
  y.domain([0, 1]);
  //y.domain(d3.extent(lineData, function(d) { return d.value; }));
  context.beginPath();
  line(lineData);
  context.lineWidth = 1.5;
  context.strokeStyle = "steelblue";
  context.stroke();
}

var VideoTimeline = React.createClass({
  getInitialState: function() {
    return {
      width: 0,
      selectedFrame: -1,
    };
  },
  posToFrameNumber: function(pageX) {
    var axis = $(this.refs.axis);
    var offset = axis.offset();
    var xPos = pageX - offset.left;
    var numTicks = Math.floor(this.state.width / 100);
    var axisWidth = numTicks * 100;
    var percent = Math.min(xPos / axisWidth, 1);
    var frame = Math.min(Math.floor(this.props.video.frames * percent),
                         this.props.video.frames - 1);
      console.log(frame);
    return frame;
  },
  handleMouseMove: function(e) {
    var targetedFrame = this.posToFrameNumber(e.pageX);

    this.state.onSelectedFrameChange({
      videoId: this.props.video.id,
      frame: targetedFrame,
    });
  },
  handleClick: function(e) {
    var targetedFrame = this.posToFrameNumber(e.pageX);

    this.state.onSelectedFrameChange({
      videoId: this.props.video.id,
      frame: targetedFrame,
    });
    this.setState({selectedFrame: targetedFrame});
  },
  handleMouseLeave: function(e) {
    this.state.onSelectedFrameChange({
      videoId: this.props.video.id,
      frame: this.state.selectedFrame,
    });
  },
  componentDidMount: function() {
    var width = $(ReactDOM.findDOMNode(this)).width();
    var onSelectedFrameChange =
      _.debounce(this.props.onSelectedFrameChange, 50);
    this.setState({
      width: width,
      onSelectedFrameChange: onSelectedFrameChange
    });
  },
  componentDidUpdate: function() {
      var context = ReactDOM.findDOMNode(this.refs.canvas).getContext("2d");

      var plotWidth = this.state.width;
      var plotHeight = 80;
      context.clearRect(0, 0, plotWidth, plotHeight);

      var margin = {top: 0, right: 0, bottom: 0, left: 0},
          canvasWidth = plotWidth - margin.left - margin.right,
          canvasHeight = plotHeight - margin.top - margin.bottom;

      var x = d3.scaleLinear()
                .range([0, canvasWidth]);

      var y = d3.scaleLinear()
                .range([canvasHeight, 0]);

      var line = d3.line()
                   .x(function(d) { return x(d.frame); })
                   .y(function(d) { return y(d.value); })
                   .curve(d3.curveStep)
                   .context(context);

      context.translate(margin.left, margin.top);

      //var lineData = jobMetadata.graphics.plotValues(predictionData);
      var lineData = _.map(this.props.plotValues, function(d, i) {
          return {frame: i, value: d};
      });

      //x.domain(d3.extent(confusionData, function(d) { return d.frame; }));
      x.domain([0, this.props.video.frames]);
      y.domain([0, _.max(this.props.plotValues)]);
      //y.domain(d3.extent(lineData, function(d) { return d.value; }));
      context.beginPath();
      line(lineData);
      context.lineWidth = 1.5;
      context.strokeStyle = "steelblue";
      context.stroke();
  },
  componentWillReceiveProps: function(nextProps) {
    var onSelectedFrameChange =
      _.debounce(this.props.onSelectedFrameChange, 50);
    this.setState({
      onSelectedFrameChange: onSelectedFrameChange
    });
  },
  render: function() {
    var video = this.props.video;

    var tickWidth = 100;
    var tickHeight = 80;

    var labelWidth = 50;
    var labelHeight = 20;

    var numTicks = Math.floor(this.state.width / tickWidth);
    var ticks = _.times(numTicks, function(i) {
      var style = {
        left: tickWidth * i,
        width: tickWidth - 1,
        top: 0,
        height: tickHeight,
      };
      return (
        <div className="timeline-tick"
             style={style}
             key={i}>
        </div>
      );
    });
    var style = {
      left: tickWidth * numTicks - 1,
      width: 0,
      top: 0,
      height: tickHeight,
    };
    var lastTick = (
      <div className="timeline-tick"
           style={style}
           key={numTicks}>
      </div>
    );
    ticks.push(lastTick);
    var tickLabels = _.times(numTicks, function(i) {
      var style = {
        left: tickWidth * i - labelWidth / 2,
        width: labelWidth,
        top: tickHeight,
        height: labelHeight,
      };
      return (
        <div className="timeline-tick-label"
             style={style}
             key={i}>
          {Math.round(video.frames / numTicks * i)}
        </div>
      );
    });
    var style = {
      left: tickWidth * numTicks - labelWidth / 2,
      width: labelWidth,
      top: tickHeight,
      height: labelHeight,
    };
    var lastTickLabel = (
      <div className="timeline-tick-label"
           style={style}
           key={numTicks}>
        {video.frames}
      </div>
    );
    tickLabels.push(lastTickLabel);

    var plotWidth = tickWidth * numTicks;
    var plotHeight = tickHeight;
    var plotStyle = {
      width: plotWidth,
      height: plotHeight,
    };
    var plotCanvas = (
        <canvas className="timeline-plot"
                width={plotWidth}
                height={plotHeight}
                style={plotStyle}
                ref="canvas">
        </canvas>
    );
    return (
      <div className="video-timeline"
           onClick={this.handleClick}
           onMouseMove={this.handleMouseMove}
           onMouseLeave={this.handleMouseLeave}>
        <div className="timeline-axis"
             ref="axis">
          {ticks}
          {tickLabels}
          {plotCanvas}
        </div>
      </div>
    )
  }
});

var VideoNavigator = React.createClass({
  render: function() {
    return (
      <div className="video-navigator">
        <VideoTimeline job={this.props.job}
                       video={this.props.video}
                       plotValues={this.props.plotValues}
                       onSelectedFrameChange={
                         this.props.onSelectedFrameChange}/>
      </div>
    );
  }
});

var VideoBrowser = React.createClass({
  render: function() {
    var job = this.props.job;
    var onSelectedFrameChange = this.props.onSelectedFrameChange;
    var videoNavigators = _.map(
        _.zip(this.props.videos, this.props.videoPlotValues),
        function(video)
    {
      return (
        <VideoNavigator job={job}
                        video={video[0]}
                        plotValues={video[1]}
                        onSelectedFrameChange={onSelectedFrameChange}
                        key={video[0]['id']}/>
      );
    });

    return (
      <div className="video-browser">
        {videoNavigators}
      </div>
    );
  }
});

var ViewerPanel = React.createClass({
  getInitialState: function() {
    return {
      threshold: 0.3,
      plotType: 'certainty',
    };
  },
  handleThresholdChange: function(e) {
    this.setState({threshold: e.target.value});
  },
  handlePlotTypeChange: function(e) {
    this.setState({plotType: e.target.value});
  },
  handleVideoResize: function(e) {
    this.props.graphics.teardown(this.refs.container);
    var videoElement = ReactDOM.findDOMNode(this.refs.video)
                               .getElementsByTagName('video')[0];
    this.props.graphics.setup(this.refs.container, videoElement);
  },
  componentDidMount: function() {
    var frameIndicator = $('<div/>', {'id': 'frame-indicator',
                                      'class': 'timeline-pos-indicator'})
      .css('left', "50%")
      .hide();
    $("#main-panel").append(frameIndicator);

    var classIndicator = $('<div/>', {'id': 'class-indicator',
                                      'class': 'timeline-pos-indicator'})
      .css('left', "50%")
      .hide();
    $("#main-panel").append(classIndicator);

    var videoElement = ReactDOM.findDOMNode(this.refs.video)
                               .getElementsByTagName('video')[0];
    this.props.graphics.setup(this.refs.container, videoElement);
    videoElement.addEventListener('resize', this.handleVideoResize);
  },
  componentDidUpdate: function(prevProps, prevState) {
    var videoElement = ReactDOM.findDOMNode(this.refs.video)
                               .getElementsByTagName('video')[0];
    // Setup new graphics if it changes
    if (prevProps.graphics != this.props.graphics) {
      prevProps.graphics.teardown(this.refs.container);
      console.log(videoElement);
      this.props.graphics.setup(this.refs.container, videoElement);
    }
    if (prevProps.video.mediaPath != this.props.video.mediaPath) {
      this.refs.video.load();
    }
    if (this.props.selectedFrame) {
      if (this.props.selectedFrame.data.hasOwnProperty('time')) {
        this.refs.video.seek(this.props.selectedFrame.data.time);
      }
      if (this.props.selectedFrame.status == 'valid') {
        console.log(this.props.selectedFrame.data);
          var color;
          if (this.props.job["name"] == "bboxes_test") {
              color = "yellow";
          } else {
              color = "red";
          }
        this.props.graphics.draw(
          videoElement,
          this.props.video,
          this.props.selectedFrame.data.frame + 'b',
          this.props.selectedFrame.data.data.base_bboxes,
          'base',
          color);
          if (color != "yellow" && this.props.selectedFrame.data.data.hasOwnProperty("tracked_bboxes")) {
              this.props.graphics.draw(
                  videoElement,
                  this.props.video,
                  this.props.selectedFrame.data.frame + 'g',
                  this.props.selectedFrame.data.data.tracked_bboxes,
                  'tracked',
                  'green');
          } else {
              this.props.graphics.draw(
                  videoElement,
                  this.props.video,
                  this.props.selectedFrame.data.frame + 'g',
                  [],
                  'tracked',
                  'green');
          }
      }
    }
  },
  handleReset: function() {
    this.props.handleReset();
  },
  render: function() {
    var frame = this.props.selectedFrame;
    var frameNum = frame ? (frame.data.frame ? ('Frame ' + frame.data.frame) : '') : '';
    return (
      <div className="viewer-panel" ref="container">
        <Video id="video-viewer" width="100%" ref="video">
          <source src={this.props.video.mediaPath} type="video/mp4" />
        </Video>
        <div className="viewer-controls">
          <div className="controls-left">
            {frameNum}
          </div>
          <button type="button" className="controls-right"onClick={this.handleReset}>
            Demo Jump
          </button>
        </div>
      </div>
    );
  }
});

var VisualizerApp = React.createClass({
  getInitialState: function() {
    return {
      datasets: [{
        id: -1,
        name: "Loading...",
      }],
      selectedDataset: -1,
      videos: [{
        frames: 1,
        width: 1280,
        height: 720,
        id: -1,
        mediaPath: '',
        name: "Loading...",
      }],
      videoPlotValues: [
        []
      ],
      selectedVideo: -1,
      jobs: [{
        id: -1,
        name: "Loading...",
      }],
      selectedJob: -1,
      frameData: [
        [{
          status: 'invalid',
          data: {},
        }]
      ],
      selectedFrame: 0,
      height: 0,
    };
  },
  findDataset: function(id) {
    return _.find(this.state.datasets, function(d) { return d.id == id; });
  },
  findVideo: function(id) {
    return _.find(this.state.videos, function(d) { return d.id == id; });
  },
  findVideoIndex: function(id) {
    return _.findIndex(this.state.videos, function(d) {
      return d.id == id;
    });
  },
  findJob: function(id) {
    return _.find(this.state.jobs, function(d) { return d.id == id; });
  },
  handleSelectedFrameChange: function(d) {
    var frame = d.frame;

    if (this.state.selectedJob != -1) {
      this.loadPredictionData(
          d.videoId, this.state.selectedJob, frame - 1, frame + 1);
    }
    this.setState({
      selectedVideo: d.videoId,
      selectedFrame: d.frame
    });
  },
  handleSelectedDatasetChange: function(datasetId) {
    $.ajax({
      url: "datasets/" + datasetId + "/jobs",
      dataType: "json",
      success: function(jobsData) {
        this.setState({jobs: jobsData});
        //jobMetadata = data[0];
        //jobMetadata.graphics =
        //  graphicsOptions[jobMetadata.featureType];
        //jobMetadata.graphics.setup($("#main-panel"));
      }.bind(this),
    });
    $.ajax({
      url: "datasets/" + datasetId + "/videos",
      dataType: "json",
      success: function(videosData) {
        var frameData = _.map(videosData, function(video) {
          return _.map(video.times, function(t) {
            return {status: 'invalid', data: {time: t}};
          });
        })
          this.setState({
            selectedVideo: videosData[0].id,
            selectedFrame: 0,
            videos: videosData,
            videoPlotValues: _.times(_.size(videosData), function(i) {
                return [];
            }),
            frameData: frameData
          });
      }.bind(this)
    });
    this.setState({
      selectedDataset: datasetId,
      selectedJob: -1,
    });
  },
  handleSelectedJobChange: function(jobId) {
    var frameData = _.map(this.state.videos, function(video) {
      return _.times(video.frames, function(i) {
        return {status: 'invalid', data: {}};
      });
    })

    this.setState({
      selectedJob: jobId,
      frameData: frameData
    });
    this.loadPlotValues(jobId);
  },
  handleKeyPress: function(event) {
      var selectedFrame = this.state.selectedFrame;
      var videoId = this.state.selectedVideo;
      if (event.keyCode == 37) {
          // left
          this.handleSelectedFrameChange({
              videoId: videoId,
              frame: selectedFrame - 1,
          });
      } else if (event.keyCode == 39) {
          // right
          this.handleSelectedFrameChange({
              videoId: videoId,
              frame: selectedFrame + 1,
          });
      }
  },
  handleReset: function() {
      var selectedFrame = this.state.selectedFrame;
      var videoId = this.state.selectedVideo;

      this.handleSelectedFrameChange({
          videoId: videoId,
          frame: 4600,
      });

  },
  loadPlotValues: function(jobId) {
    var promises = _.map(this.state.videos, function(video) {
        return this.loadPredictionData(video.id, jobId, 0, video.frames);
    }.bind(this));
    $.whenall(promises).done(function(data) {
       this.setState({
         videoPlotValues: _.map(data, function(p) {
            var vals = p[0];
            return _.map(vals, function(perFrame) {
                return _.size(perFrame.data.base_bboxes) +
                       _.size(perFrame.data.tracked_bboxes);
            });
          }),
       })
     }.bind(this));

  },
  loadPredictionData: function(videoId, jobId, start, end) {
    var frameData = this.state.frameData[videoId];

    var foundStart = false;
    var requestStart = start;
    var requestEnd = end;
    for (var i = start; i < end; ++i) {
      if (frameData[i].status != 'invalid') {
        if (foundStart) {
          requestEnd = i;
          break;
        } else {
          requestStart = i + 1;
        }
      } else {
        foundStart = true;
        frameData =
          update(frameData, {[i]: {status: {$set: 'loading'}}});
      }
    }
    // The entire range is already loaded so we don't need to send a request
    if (requestStart == requestEnd) return;
      console.log(this.findJob(jobId))
      var columns;
      if (this.findJob(jobId)["name"] == "facenet_eating_bg") {
          columns = "base_bboxes,tracked_bboxes";
      } else {
          columns = "base_bboxes";
      }
    var p = $.ajax({
      url: "datasets/" + this.state.selectedDataset +
           "/jobs/" + jobId +
           "/features/" + videoId,
      dataType: "json",
      data: {
        columns: columns,
        start: requestStart,
        end: requestEnd,
        stride: 1,
        category: -1,
        threshold: $("#threshold-input").val(),
      }
    });
    p.done(function(data) {
      for (var i = 0; i < (requestEnd - requestStart); ++i) {
        var frame = requestStart + i;
        frameData = update(frameData, {
          [frame]: {
            status: {$set: 'valid'},
            data: {$set: data[i]}
          }
        });
      }
      this.setState({
        frameData: update(this.state.frameData,
                          {[videoId]: {$set: frameData}})
      });
    }.bind(this));
    return p;
  },
  componentDidMount: function() {
    this.setState({height: this._getHeight()});
    window.addEventListener('resize', this._onResize);
    $(document).on("keydown", this.handleKeyPress);

    $.ajax({
      url: "datasets",
      dataType: "json",
      success: function(datasetsData) {
        this.setState({datasets: datasetsData});
      }.bind(this),
    });
  },
  _onResize: function() {
    this.setState({height: this._getHeight()});
  },
  _getHeight: function() {
    var top = this.refs.body ? $(this.refs.body).offset().top : 0;
    return window.innerHeight - top;
  },
  render: function() {
    var datasetDivs = _.map(this.state.datasets, function(dataset) {
      var cls = "dataset-info " + (this.state.selectedDataset == dataset.id ? 'active' : '');
      return (
        <div className={cls}
             key={dataset.id}
             onClick={()=>this.handleSelectedDatasetChange(dataset.id)}>
          {dataset.name}
        </div>
      );
    }.bind(this));
    var jobDivs = _.map(this.state.jobs, function(job) {
      var cls = "job-info " + (this.state.selectedJob == job.id ? 'active' : '');
      return (
        <div className={cls}
             key={job.id}
             onClick={()=>this.handleSelectedJobChange(job.id)}>
          {job.name}
        </div>
      );
    }.bind(this));
    var selectedVideo = this.state.selectedVideo;
    var selectedVideoIndex = this.findVideoIndex(selectedVideo);
    var selectedFrame = this.state.selectedFrame;
    var frame =
      this.state.frameData[selectedVideoIndex][selectedFrame];
    return (
        <div className="visualizer-app" onKeyDown={this.handleKeyPress}>
        <div className="header">
          <h1>Scanner</h1>
          <div className="dataset-panel">
            <h2>Datasets</h2>
            {datasetDivs}
          </div>
          <div className={"job-panel " + (this.state.selectedDataset == -1 ? 'hidden' : '')}>
            <h2>Jobs</h2>
            {jobDivs}
          </div>
        </div>
        <div className="body" ref="body" style={{height: this.state.height}}>
          <VideoBrowser job={this.findJob(this.state.selectedJob)}
                        videos={this.state.videos}
                        videoPlotValues={this.state.videoPlotValues}
                        onSelectedFrameChange={this.handleSelectedFrameChange} />
          <ViewerPanel job={this.findJob(this.state.selectedJob)}
                       video={this.findVideo(selectedVideo)}
                       graphics={detectionGraphics}
                       selectedFrame={frame}
                       handleReset={this.handleReset}/>
        </div>
      </div>
    );
  }
});

$(document).ready(function() {
  ReactDOM.render(<VisualizerApp />, $('#app')[0]);
});