package
{
    import flash.display.DisplayObject;
    import flash.display.DisplayObjectContainer;
    import flash.display.Loader;
    import flash.display.LoaderInfo;
    import flash.display.MovieClip;
    import flash.display.Sprite;
    import flash.geom.Point;
    import flash.geom.Rectangle;
    import flash.events.Event;
    import flash.events.MouseEvent;
    import flash.text.TextField;
    import flash.text.TextFormat;
    import flash.events.TimerEvent;
    import flash.media.SoundMixer;
    import flash.media.SoundTransform;
    import flash.external.ExternalInterface;
    import flash.system.ApplicationDomain;
    import flash.system.Security;
    import flash.utils.Dictionary;
    import flash.utils.describeType;
    import flash.utils.getDefinitionByName;
    import flash.utils.getQualifiedClassName;
    import flash.utils.Timer;
    import flash.utils.getTimer;

    /**
     * Injected reflection bridge.
     *
     * The host splices this class into the movie and places one instance on the
     * first frame. It is not the document class. The instance keeps the stage,
     * which is how later calls reach classes loaded by the preloader.
     */
    public class DobotBridge extends Sprite
    {
        // Versioned compact field dictionary sent with the compact entity rows.
        private static var _packedFields:Array=["isNpc","isHero","cargo","credits","uridium","experience","honor","localTargetId","speed","isValid","isOwned","attackingHero","attackingTargetId","health","maxHealth","shield","maxShield","nanohullHP","nanohullMaxHP","isCollectable","hash","typeID","entityKind"];
        private static var _packedIndices:Object=packedIndices();
        private static function packedIndices():Object {
            var result:Object={};
            for (var i:int=0;i<_packedFields.length;i++) result[_packedFields[i]]=i;
            return result;
        }
        private static var _domains:Array = [];
        private static var _domainAt:int = -2000;
        private static var _classes:Object = {};
        private static var _bindings:Object = {};
        private static var _frameQueue:Array = [];
        private static var _results:Object = {};
        // Host settings (InProcessPlayer setMaxFps/setVolume). 60 is the game rate.
        private static var _targetFps:int = 60;
        private static var _volumeApplied:Boolean = false;
        private static var _settingsLoaded:Boolean = false;
        // Direct-action targets, rebuilt when the ScreenManager or map changes.
        private static var _direct:Object = null;
        private static var _signatures:Object = {};
        private static var _frames:Number = 0;
        private static var _captureAt:int = 0;
        private static var _captureEnabled:Boolean=true;
        private static var _pepperNativeMode:Boolean=false;
        private static var _navObservedAt:Number=0;
        private static var _nativeMode:Boolean=false;
        private static var _backendConfigured:Boolean=false;
        // Native discovery root; see NativeAnchor below and native_state.cpp.
        private static var _anchor:NativeAnchor;
        private static var _captureLastMs:int=0;
        private static var _items:Object={};
        private static var _itemsAt:int=-1000;
        private static var _catalogAt:int=-1000;
        private static var _catalogSent:Boolean=false;
        private static var _navigationLastMs:int=0;
        private static var _navAt:int=-1000;
        private static var _navMap:int=-999;
        private static var _navigation:Object;
        private static var _starSystem:Object;
        private static var _fps:Number = 0;
        private static var _fpsAt:int = 0;
        private static var _fpsFrames:Number = 0;
        private static var _clip:DobotBridge;
        private static var _overlayLayer:Sprite;
        private static var _overlays:Object={};
        private static var _timer:Timer;
        private static var _stateFields:Object = {};
        // Bounded event journal. Each GuiNotifications.LOG_MESSAGE notification
        // becomes exactly one game_log event (duplicates stay distinct), kept in
        // arrival order with a monotonic sequence and at most 4096 retained.
        private static var _events:Array=[];
        private static var _eventSeq:Number=0;
        private static var _logHooked:Boolean=false;
        private static var _logObserver:Object=null;
        private static var _rewardTemplates:Array=[];
        private static var _templatesDone:Boolean=false;
        private static var _ready:Boolean = false;
        private static var _booted:Boolean = boot();

        public function DobotBridge()
        {
            mouseEnabled = false;
            mouseChildren = false;
            tabEnabled = false;
            addEventListener(Event.ENTER_FRAME, tick);
            _clip = this;
            if (stage == null)
            {
                addEventListener(Event.ADDED_TO_STAGE, onAdded);
            }
        }

        private function onAdded(event:Event):void
        {
            removeEventListener(Event.ADDED_TO_STAGE, onAdded);
        }

        private static function boot():Boolean
        {
            tryRegister();
            try
            {
                _timer = new Timer(250, 120);
                _timer.addEventListener(TimerEvent.TIMER, onTimer);
                _timer.start();
            }
            catch (error:Error)
            {
            }
            return true;
        }

        private static function onTimer(event:TimerEvent):void
        {
            arm();
            tryRegister();
            if (_ready && _timer != null)
            {
                _timer.stop();
            }
        }

        private static function arm():void
        {
            var root:DisplayObject = displayRoot();
            if (root == null)
            {
                return;
            }
            if (!root.hasEventListener(Event.ENTER_FRAME))
            {
                root.addEventListener(Event.ENTER_FRAME, onFrame);
            }
            if (root.stage == null && !root.hasEventListener(Event.ADDED_TO_STAGE))
            {
                root.addEventListener(Event.ADDED_TO_STAGE, onFrame);
            }
        }

        private static function onFrame(event:Event):void
        {
            tryRegister();
        }

        private static function tryRegister():void
        {
            if (_ready || !ExternalInterface.available)
            {
                return;
            }
            try
            {
                Security.allowDomain("*");
            }
            catch (error:Error)
            {
            }
            try
            {
                Security.allowInsecureDomain("*");
            }
            catch (error:Error)
            {
            }
            ExternalInterface.addCallback("pingBridge", function():String
            {
                return "dobot-bridge";
            });
            ExternalInterface.addCallback("invokeAS", function(
                targetPath:String, method:String, jsonArgs:String):String
            {
                return invoke(targetPath, method, jsonArgs);
            });
            ExternalInterface.addCallback("inspectAS", function(targetPath:String):String
            {
                return inspect(targetPath);
            });
            ExternalInterface.addCallback("stateAS", function(targetPath:String):String
            {
                return state(targetPath);
            });
            ExternalInterface.addCallback("configureAS",function(enabled:String):String { _captureEnabled=enabled=="1";return "configured"; });
            ExternalInterface.addCallback("configureBackendAS",function(mode:String):String {
                if(mode!="native" && mode!="cached") throw new Error("Unknown capture backend");
                _nativeMode=mode=="native";_backendConfigured=true;
                _captureAt=-1000;_navAt=-1000;_navMap=-999;_navigation=null;
                return mode;
            });
            ExternalInterface.addCallback("navigationAS", function():String {
                return JSON.stringify({status:"ok",data:{navigation:_navigation,
                    observedAtUnixMs:_navObservedAt,frames:_frames,fps:_fps,
                    navigationMs:_navigationLastMs}});
            });
            ExternalInterface.addCallback("captureAS", capture);
            ExternalInterface.addCallback("referenceAS", function():String {
                var value:Object=JSON.parse(capture());
                if(value.status=="ok") {
                    var navigation:Object=navigationSnapshot();
                    navigation.star_system=starSystem();navigation.hero.items=_items;
                    value.data.navigation=navigation;
                }
                return JSON.stringify(value);
            });
            ExternalInterface.addCallback("settingsAS", function(fps:String, volume:String):String {
                applySettings(Number(fps), Number(volume));
                _settingsLoaded = true;
                return "configured";
            });
            ExternalInterface.addCallback("queueAS", queueCall);
            ExternalInterface.addCallback("resultAS", takeResult);
            _ready = true;
        }


        // The host scans for the anchor's per-session nonce instead of a
        // game-value fingerprint. Only our own object is written; the
        // ScreenManager reference comes from its public getInstance().
        private static function plantAnchor():void
        {
            if (_anchor == null) {
                var parts:Array = String(ExternalInterface.call("xenomitAnchor")).split(",");
                if (parts.length != 2) return;
                var anchor:NativeAnchor = new NativeAnchor();
                anchor.tag0 = Number(parts[0]);
                anchor.tag1 = Number(parts[1]);
                if (isNaN(anchor.tag0) || isNaN(anchor.tag1)) return;
                _anchor = anchor;
            }
            if (_anchor.screen == null || (_frames & 63) == 0)
                _anchor.screen = resolvePath("net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
        }

        private function tick(event:Event):void
        {
            // Timelines can recreate the injected sprite. Only the current
            // instance may capture, execute calls, or contribute frame counts.
            if (this !== _clip) { removeEventListener(Event.ENTER_FRAME,tick);return; }
            _frames++;
            // Retry the log journal hook until the game facade/view exists so
            // events accumulate without waiting for the first clientEvents call.
            if (!_logHooked && (_frames & 31) == 0) { try { ensureLogHook(); } catch (ignoredHook:Error) {} }
            if (!_backendConfigured && ExternalInterface.available) {
                var backend:String=String(ExternalInterface.call("xenomitBackend"));
                _pepperNativeMode=backend=="pepper-native";
                _nativeMode=backend=="native" || _pepperNativeMode;
                _backendConfigured=true;
            }
            if (!_pepperNativeMode && _nativeMode && _captureEnabled && ExternalInterface.available) {
                try { plantAnchor(); } catch (ignoredAnchor:Error) {}
                ExternalInterface.call("xenomitNativeFrame");
            }
            if (!_settingsLoaded && ExternalInterface.available) {
                _settingsLoaded = true;
                try {
                    var settings:Array = String(ExternalInterface.call("xenomitSettings")).split(",");
                    if (settings.length == 2) applySettings(Number(settings[0]), Number(settings[1]));
                } catch (ignoredSettings:Error) {}
            }
            if (stage != null && stage.frameRate != _targetFps) stage.frameRate = _targetFps;
            var now:int = getTimer();
            if (now - _fpsAt >= 1000) {
                _fps = (_frames - _fpsFrames) * 1000 / (now - _fpsAt);
                _fpsFrames = _frames; _fpsAt = now;
            }
            // Spread navigation and entity serialization across different frames.
            // Combining both bursts can exceed the 16.7 ms rendering budget.
            // Pepper captures natively. Synchronous outgoing EI callbacks can
            // deadlock with incoming command calls in its browser IPC.
            if (!_pepperNativeMode) {
            var navigationFrame:Boolean=false;
            if (_captureEnabled && ExternalInterface.available && now-_itemsAt>=1000) {
                _itemsAt=now;
                try { navigationFrame=pushItems(); } catch (ignoredItems:Error) {}
            }
            if (_captureEnabled && !_catalogSent && ExternalInterface.available && now-_catalogAt>=1000) {
                _catalogAt=now;
                try {
                    var catalog:Object=starSystem();
                    if (catalog.maps.length>0) {
                        ExternalInterface.call("xenomitCatalog",JSON.stringify(catalog));
                        _catalogSent=true;navigationFrame=true;
                    }
                } catch (ignoredCatalog:Error) {}
            }
            if (!navigationFrame && _captureEnabled && ExternalInterface.available && (_navigation==null || now-_navAt>=300)) {
                var manager:Object=resolvePath("net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
                var currentMap:Object=member(manager,"map");
                var currentId:int=currentMap==null ? -1 : int(member(currentMap,"mapID"));
                if (_navigation==null || currentId!=_navMap || now-_navAt>=300) {
                    var navigationStart:int=getTimer();
                    try {
                        pushNavigation(currentId);
                        _navigationLastMs=getTimer()-navigationStart;
                        navigationFrame=true;
                    } catch (ignoredNavigation:Error) {}
                }
            }
            if (!navigationFrame && now-_captureAt>=(_captureEnabled && !_nativeMode ? 150 : 1000) && ExternalInterface.available) {
                _captureAt=now;
                var captureStart:int=getTimer();
                try {
                    if (_captureEnabled && !_nativeMode) ExternalInterface.call("xenomitCapture",capture());
                    else ExternalInterface.call("xenomitMetrics",JSON.stringify({frames:_frames,fps:_fps}));
                } catch (ignored:Error) {}
                _captureLastMs=getTimer()-captureStart;
            }
            }
            if (_pepperNativeMode && _captureEnabled && now-_navAt>=300) {
                var pepperStart:int=getTimer();
                _navigation=null;_navObservedAt=0;_navAt=now;
                try {
                    var pepperNavigation:Object=navigationSnapshot();
                    _navigation=pepperNavigation;_navObservedAt=new Date().time;
                    _navigationLastMs=getTimer()-pepperStart;
                } catch (ignoredPepperNavigation:Error) {}
            }
            // Bounded FIFO: up to four operations within 4 ms per game frame. The
            // host is told each result at once (xenomitResult); resultAS stays
            // as the fallback when the host does not accept the push.
            var executed:int = 0;
            var budgetStart:int = getTimer();
            while (_frameQueue.length > 0 && executed < 4 && (executed == 0 || getTimer() - budgetStart < 4)) {
                var q:Object = _frameQueue.shift();
                executed++;
                var value:String = getTimer() > q.deadline ?
                    fail("frame call expired before execution") : invoke(q.target,q.method,q.args);
                var pushed:Boolean = false;
                if (ExternalInterface.available) {
                    try { pushed = String(ExternalInterface.call("xenomitResult", q.id, value)) == "accepted"; }
                    catch (ignoredPush:Error) {}
                }
                if (!pushed) _results[q.id] = {at:now, value:value};
            }
            for (var key:String in _results) if (now - _results[key].at > 10000) delete _results[key];
        }
        private static function applySettings(fps:Number, volume:Number):void
        {
            _targetFps = fps >= 1 && fps <= 100 ? int(fps) : 60;
            if (!isNaN(volume) && volume >= 0 && volume <= 100 && (_volumeApplied || volume < 100)) {
                SoundMixer.soundTransform = new SoundTransform(volume / 100);
                _volumeApplied = true;
            }
        }

        private static function directTargets():Object
        {
            var screen:Object = resolvePath("net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
            var map:Object = member(screen, "map");
            if (_direct == null || _direct.screen !== screen || _direct.map !== map) {
                _direct = {screen:screen, map:map, events:member(screen, "eventManager"),
                    ships:map == null ? null : callMember(map, "getShipManager"),
                    facade:resolvePath("net.bigpoint.darkorbit.mvc.application.GameFacade.getInstance()")};
            }
            return _direct;
        }

        private static function required(target:Object, name:String):Object
        {
            if (target == null) throw new Error(name + " unavailable");
            return target;
        }

        // InProcessPlayer direct interactions (moveShip, selectEntity, collectBox,
        // sendNotification) on the same game paths the controller used through
        // reflection. Targets are cached; entities resolve by stable ID.
        private static function direct(args:Array):Object
        {
            if (args == null || args.length < 1) throw new Error("direct action missing");
            var targets:Object;
            try { targets = directTargets(); } catch (error:Error) { _direct = null; throw error; }
            var events:Object = targets.events, ships:Object = targets.ships;
            switch (String(args[0])) {
                case "move":
                    required(events, "event manager").moveHeroToCordinates(int(args[1]), int(args[2]));
                    return null;
                case "stop":
                    required(events, "event manager").stopHero();
                    return null;
                case "collect":
                    var box:Object = resolveEntity(Number(args[1]));
                    var at:Object = args.length >= 4 ? {x:Number(args[2]), y:Number(args[3])} : member(box, "position");
                    if (at == null) throw new Error("entity has no position");
                    required(events, "event manager").moveHeroToCordinates(int(Math.round(at.x)), int(Math.round(at.y)), box);
                    return null;
                case "select":
                    var ship:Object = resolveEntity(Number(args[1]));
                    var hp:Object = member(ship, "hp");
                    required(ships, "ship manager").selectShip(int(args[1]),
                        traitNumber(hp, "hp"), traitNumber(hp, "maxHP"), traitNumber(hp, "shield"),
                        traitNumber(hp, "maxShield"), traitNumber(hp, "nanohullHP"), traitNumber(hp, "nanohullMaxHP"));
                    if (args[2] === true) required(targets.facade, "game facade").sendNotification("MapAssetNotificationAttackMapAsset");
                    return null;
                case "deselect":
                    required(ships, "ship manager").deselectSelectedShip();
                    return null;
                case "notify":
                    var notifyArgs:Array = args.length > 2 && args[2] is Array ? args[2] as Array : [];
                    var facade:Object = required(targets.facade, "game facade");
                    facade.sendNotification.apply(facade, [String(args[1])].concat(notifyArgs));
                    return null;
            }
            throw new Error("unknown direct action " + String(args[0]));
        }

        private static function traitNumber(trait:Object, name:String):Number
        {
            var value:* = nestedValue(trait, name, "value");
            return value is Number ? Number(value) : 0;
        }

        // Signature check before use (InProcessPlayer checkMethodSignature): the
        // method exists and accepts the argument count the action passes.
        private static function accepts(target:Object, name:String, count:int):Boolean
        {
            if (target == null || !(member(target, name) is Function)) return false;
            var type:String = getQualifiedClassName(target);
            var methods:Object = _signatures[type];
            if (methods == null) {
                methods = {};
                for each (var method:XML in describeType(target).method) {
                    var total:int = method.parameter.length();
                    var optional:int = method.parameter.(@optional == "true").length();
                    methods[String(method.@name)] = [total - optional, total];
                }
                _signatures[type] = methods;
            }
            var range:Array = methods[name] as Array;
            return range != null && count >= range[0] && count <= range[1];
        }

        private static function caps():Object
        {
            var targets:Object = null;
            try { _direct = null; targets = directTargets(); } catch (ignored:Error) {}
            var events:Object = targets == null ? null : targets.events;
            var ships:Object = targets == null ? null : targets.ships;
            var facade:Object = targets == null ? null : targets.facade;
            return {
                move: accepts(events, "moveHeroToCordinates", 2),
                stop: accepts(events, "stopHero", 0),
                collect: accepts(events, "moveHeroToCordinates", 3),
                select: accepts(ships, "selectShip", 7),
                deselect: accepts(ships, "deselectSelectedShip", 0),
                notify: accepts(facade, "sendNotification", 1),
                game_command: accepts(facade, "retrieveProxy", 1)
            };
        }

        private static function queueCall(id:String,target:String,method:String,args:String):String
        {
            if (_frameQueue.length >= 32 || _results[id] != null) return fail("frame queue full or duplicate ID");
            _frameQueue.push({id:id,target:target,method:method,args:args,deadline:getTimer()+4000});
            return "queued";
        }
        private static function takeResult(id:String):String
        {
            var result:Object = _results[id];
            if (result == null) return "pending";
            delete _results[id];
            return result.value;
        }
        private static function capture():String
        {
            try {
                var started:int = getTimer();
                var root:DisplayObject = displayRoot();
                var entities:Array = mapEntitiesSnapshot(true);
                var map:Object = mapSnapshot();
                // No recursive display walk while loading. Keep capture bounded.
                var mapId:int=map == null || map.id==null ? -1 : int(map.id);
                if (_navigation==null || _navMap!=mapId) {
                    pushNavigation(mapId);
                }
                return JSON.stringify({status:"ok",data:{
                    stageWidth:_clip.stage == null ? null : _clip.stage.stageWidth,
                    stageHeight:_clip.stage == null ? null : _clip.stage.stageHeight,
                    map:map,entitiesEncoding:"packed_v1",entityFields:_packedFields,entities:entities == null ? [] : entities,
                    visited:entities == null ? 0 : entities.length,
                    truncated:entities != null && entities.length >= 2000,
                    navigation:null,observedAtUnixMs:new Date().time,
                    frames:_frames,fps:_fps,captureMs:_captureLastMs,navigationMs:_navigationLastMs,modelMs:getTimer()-started}});
            } catch (error:Error) { return fail(error.message); }
            return fail("capture failed" );
        }
        private static function pushNavigation(mapId:int):void {
            _navigation=navigationSnapshot();
            ExternalInterface.call("xenomitNavigation",JSON.stringify(_navigation));
            _navAt=getTimer();_navMap=mapId;
        }
        private static function navigationSnapshot():Object
        {
            var screen:Object = resolvePath("net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
            var map:Object = member(screen,"map");
            var hero:Object = resolvePath("net.bigpoint.darkorbit.Hero");
            var ship:Object = member(map,"hero");
            var settings:Object = resolvePath("net.bigpoint.darkorbit.settings.Settings");
            var n:Object = {map_id:map == null ? null : map.mapID,geometry_ready:false,
                obstacles:[],portals:[],hero:{owns_pet:member(hero,"ownsPet") === true,
                pet_destroyed:member(hero,"isPetDestroyed") === true,
                killed:member(hero,"isKilled") === true,level:member(hero,"level"),
                configuration:member(settings,"selectedConfiguration"),
                pet_active:member(ship,"pet") != null && member(ship.pet,"container") != null}};
            var source:* = member(map,"entities");
            n.star_system=null;
            n.hero.items=null;
            // Slowly changing command/manager observations are supplementary
            // to native positions, health and destinations. Preserve them when
            // full AS entity serialization is disabled in native mode.
            if (_nativeMode && ship != null) {
                var heroFields:Object={};
                putNumber(heroFields,"cargo",nestedValue(hero,"cargo","value"));
                putNumber(heroFields,"credits",member(hero,"creditsAmount"));
                putNumber(heroFields,"uridium",member(hero,"uridiumAmount"));
                putNumber(heroFields,"experience",member(hero,"experiencePoints"));
                putNumber(heroFields,"honor",member(hero,"honorPoints"));
                putNumber(heroFields,"localTargetId",localTargetValue(settings));
                n.entity_fields={};n.entity_fields[String(ship.id)]=heroFields;
            }
            var menu:Object=ship == null || !n.hero.owns_pet ? null : petGearMenu();
            n.hero.pet_gears=[];
            if (menu != null && menu.itemsCount<=64) {
                for (var gearIndex:int=0;gearIndex<menu.itemsCount;gearIndex++) {
                    var gearItem:Object=menu.getElement(gearIndex);
                    if (gearItem != null) n.hero.pet_gears.push(int(gearItem.gearID));
                }
            }
            var repair:Object=freeRepair();
            if (repair != null) n.hero.free_repair_type=repair.type;
            if (source == null || member(map,"hero") == null) return n;
            n.geometry_ready = true;
            for each (var e:Object in source) {
                if (e == null) continue;
                var cls:String = getQualifiedClassName(e);
                if (_nativeMode && (cls.substr(cls.lastIndexOf("::")+2)=="Ship" || cls.substr(cls.lastIndexOf("::")+2)=="Pet")) {
                    var supplemental:Object=n.entity_fields[String(e.id)] || {};
                    putNumber(supplemental,"speed",nestedValue(member(e,"moveTrait"),"speed","value"));
                    supplemental.isValid=member(e,"container")!=null;
                    var lock:Object=member(e,"attackTarget"),lockType:*=nestedValue(lock,"lockType","value");
                    if (member(e,"isNPC") && (lockType===1 || lockType===2)) supplemental.isOwned=lockType===1;
                    var target:Object=member(member(member(e,"attackTrait"),"target"),"owner");
                    if(target!=null)supplemental.attackingHero=callBoolean(target,"isHeroShip");
                    if(e===ship)supplemental.attackingTargetId=target==null?null:member(target,"id");
                    n.entity_fields[String(e.id)]=supplemental;
                }
                var position:Object = member(e,"position");
                if (cls.indexOf("Portal") >= 0 && position != null)
                    n.portals.push({id:e.id,position:{x:position.x,y:position.y},target_map:portalTarget(starSystem(),map,e),jumping:member(e,"isJumping") === true});
                if (cls.indexOf("Zone") < 0 || member(e,"zoneType") != "NOA") continue;
                if (n.obstacles.length >= 256) { n.geometry_ready=false; continue; }
                var shape:Object = null;
                if (cls.indexOf("CircleZone") >= 0 && position != null && Number(member(e,"collisionRadius")) > 0)
                    shape={type:"circle",center:{x:position.x,y:position.y},radius:e.collisionRadius};
                else if (cls.indexOf("RectangleZone") >= 0) {
                    var b:Object=member(e,"collisionBounds");
                    if (b != null && b.width > 0 && b.height > 0)
                        shape={type:"rectangle",bounds:{x:b.x,y:b.y,width:b.width,height:b.height}};
                } else {
                    var points:*=member(e,"collisionPoints");
                    if (points != null && points.length >= 3 && points.length <= 1024) {
                        var out:Array=[];
                        for each (var pt:Object in points) out.push({x:pt.x,y:pt.y});
                        shape={type:"polygon",points:out};
                    }
                }
                if (shape == null) n.geometry_ready=false;
                else n.obstacles.push({id:e.id,name:String(member(e,"zoneId") || "No-access zone"),kind:"no_access",shape:shape,inverted:member(e,"inverted") === true});
            }
            return n;
        }


        // Inventory changes infrequently. Keep existing objects and avoid JSON
        // serialization/EI delivery when all public item flags are unchanged.
        private static function pushItems():Boolean {
            var screen:Object=resolvePath("net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
            var map:Object=member(screen,"map");
            var facade:Object=member(map,"hero")==null ? null : resolvePath("net.bigpoint.darkorbit.mvc.application.GameFacade.getInstance()");
            var proxy:Object=facade==null ? null : facade.retrieveProxy("ItemsControlMenuProxy");
            var categories:*=member(member(proxy,"categoryBar"),"categories");
            var seen:Object={};
            var changed:Boolean=false;
            if (categories!=null && categories.length<=128) for each (var category:Object in categories) {
                var items:*=member(category,"items");
                if (items==null || items.length>256) continue;
                for each (var item:Object in items) {
                    var id:String=String(item.id);
                    seen[id]=true;
                    var old:Object=_items[id];
                    var quantity:*=item.counterValue;
                    if (quantity==undefined || (quantity is Number && !isFinite(Number(quantity)))) quantity=null;
                    var available:Boolean=Boolean(item.available),activatable:Boolean=Boolean(item.activatable);
                    var selected:Boolean=Boolean(item.selected),blocked:Boolean=Boolean(item.blocked);
                    if (old==null || old.quantity!==quantity || Boolean(old.available)!=available || Boolean(old.activatable)!=activatable || Boolean(old.selected)!=selected || Boolean(old.blocked)!=blocked) {
                        var state:Object={quantity:quantity};
                        if (available) state.available=true;
                        if (activatable) state.activatable=true;
                        if (selected) state.selected=true;
                        if (blocked) state.blocked=true;
                        _items[id]=state;changed=true;
                    }
                }
            }
            for (var key:String in _items) if (!(key in seen)) {delete _items[key];changed=true;}
            if (changed) ExternalInterface.call("xenomitItems",JSON.stringify(_items));
            else ExternalInterface.call("xenomitItemsFresh","fresh");
            return changed;
        }

        private static function portalTarget(system:Object,map:Object,portal:Object):Object {
            for each (var link:Object in system.links)
                if (link.from==map.mapID && (link.portal_id==portal.id || link.portal_id==member(portal,"typeID"))) return link.to;
            return null;
        }
        private static function starSystem():Object
        {
            if (_starSystem != null) return _starSystem;
            var space:Object=resolvePath("net.bigpoint.darkorbit.mvc.gui.windows.spacemap.model.SpaceMapConfig");
            var mapClass:Object=resolvePath("net.bigpoint.darkorbit.map.model.Map");
            var xml:XML=member(space,"spaceMapConfig") as XML;
            var result:Object={maps:[],links:[]};
            if (xml==null || mapClass==null) return result;
            var ids:Object={};
            for each (var mapNode:XML in xml.descendants("map")) ids[int(mapNode.@id)]=true;
            for each (var gate:XML in xml.descendants("jumpgate")) {
                var from:int=int(gate.@ref),to:int=int(gate.leadsToMap),key:int=int(gate.@key);
                if (from>0 && to>0 && from!=to) {
                    ids[from]=true;ids[to]=true;
                    result.links.push({from:from,to:to,portal_id:key});
                }
            }
            for (var id:String in ids) {
                var descriptor:XML=mapClass.getStaticMapDescriptor(int(id));
                if (descriptor!=null) result.maps.push({id:int(id),name:String(descriptor.@name),min_level:int(descriptor.minLevel)});
            }
            if (result.maps.length>0) _starSystem=result;
            return result;
        }
        // Read the same public widgets the game populates from server offers.
        // This avoids native offsets and access to another class's private slots.
        private static function displayNodes(root:DisplayObjectContainer,limit:int=1024):Array
        {
            if (root == null) return [];
            var nodes:Array=[root];
            for (var i:int=0;i<nodes.length && nodes.length<limit;i++) {
                var parent:DisplayObjectContainer=nodes[i] as DisplayObjectContainer;
                if (parent == null) continue;
                for (var j:int=0;j<parent.numChildren && nodes.length<limit;j++) nodes.push(parent.getChildAt(j));
            }
            return nodes;
        }
        private static function petGearMenu():Object
        {
            var pet:Object=resolvePath("net.bigpoint.darkorbit.net.PetAssembly.getInstance()");
            var window:Object=member(pet,"petWindow");
            var content:Object=callMember(window,"getPetWindowContainer");
            if (content == null) return null;
            for each (var d:DisplayObject in displayNodes(content.parent as DisplayObjectContainer)) {
                if (getQualifiedClassName(d)=="net.bigpoint.darkorbit.gui.windows.components.gear::GearMenuContainer") return member(d,"mainMenu");
            }
            return null;
        }

        // Supplementary feature observations are intentionally limited to
        // public display objects and hero fields.  Native capture continues
        // to own the hot position/health path.
        private static function clientFeatures():Object
        {
            var screen:Object=resolvePath("net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
            var manager:Object=resolvePath("net.bigpoint.darkorbit.gui.GuiManager.instance");
            var windows:Object=member(manager,"windows");
            var stageRoot:DisplayObjectContainer=_clip == null ? null : _clip.stage;
            var hero:Object=resolvePath("net.bigpoint.darkorbit.Hero");
            var facade:Object=resolvePath("net.bigpoint.darkorbit.mvc.application.GameFacade.getInstance()");
            var ship:Object=member(member(screen,"map"),"hero");
            var petState:Object={};
            if (hero != null) {
                petState.owned=member(hero,"ownsPet");
                petState.destroyed=member(hero,"isPetDestroyed");
                var petEntity:Object=member(ship,"pet");
                var petActive:Boolean=member(petEntity,"container") != null;
                petState.active=ship != null && petActive;
                petState.level=member(hero,"petLevel");
                if (petActive) {
                    petState.id=member(petEntity,"id");
                    var petPosition:Object=member(petEntity,"position");
                    if (petPosition != null) petState.position={x:member(petPosition,"x"),y:member(petPosition,"y")};
                    var petLevel:*=member(petEntity,"level");
                    if (petLevel is Number) petState.level=int(petLevel);
                    var petIdle:*=member(petEntity,"idleState");
                    if (petIdle is Boolean) petState.idle=petIdle;
                    var petHp:Object=member(petEntity,"hp");
                    putNumber(petState,"health",nestedValue(petHp,"hp","value"));
                    putNumber(petState,"max_health",nestedValue(petHp,"maxHP","value"));
                    putNumber(petState,"shield",nestedValue(petHp,"shield","value"));
                    putNumber(petState,"max_shield",nestedValue(petHp,"maxShield","value"));
                    var petData:Object={};
                    var petOwner:*=member(petEntity,"ownerID");
                    if (petOwner is Number) petData.owner_id=int(petOwner);
                    var petEvasion:*=member(petEntity,"evasionProtocolEquiped");
                    if (petEvasion is Boolean) petData.evasion_protocol=petEvasion;
                    for (var pk:String in petData) { petState.data=petData; break; }
                }
                var menu:Object=petGearMenu();
                if (menu != null && menu.itemsCount<=64) {
                    petState.gears=[];
                    for (var gi:int=0;gi<menu.itemsCount;gi++) {
                        var gear:Object=menu.getElement(gi);
                        if (gear != null) petState.gears.push(int(gear.gearID));
                    }
                }
            }
            var guis:Array=[];
            if (windows != null) for (var key:String in windows) {
                var window:DisplayObject=windows[key] as DisplayObject;
                if (window != null && window.visible && window.alpha>0)
                    appendGuiTree(window,key,"",guis,stageRoot);
            }
            return {observed_at_unix_ms:new Date().time,gui:guis,pet:petState,
                gate:gateFeatures(facade),event:eventFeatures(manager,facade),
                economy:economyFeatures(hero),boosters:boosterFeatures(facade),
                capabilities:["booster.observe","gui.observe","gui.click","gui.close","ore.observe","ore.refine","ore.sell","pet.observe","economy.observe","events","gate.builder","gate.difficulty","gate.spin","gate.place","gate.buy_life","group.accept_invite"]};
        }

        private static function ggBuilderModel(facade:Object):Object
        {
            if (facade == null) return null;
            var proxy:Object=callArgs(facade,"retrieveProxy",["ggBuilder"]);
            if (proxy == null) return null;
            var model:Object=member(proxy,"viewData");
            if (model == null) model=callMember(proxy,"getData");
            return model;
        }

        // Every emitted fact comes from the ggBuilder proxy's own model;
        // nothing is inferred from window text or assumed defaults.
        private static function gateFeatures(facade:Object):Object
        {
            var model:Object=ggBuilderModel(facade);
            if (model == null) return null;
            var modelClass:Object=resolvePath("net.bigpoint.darkorbit.mvc.galaxygatebuilder.model.GalaxyGateBuilderModel");
            var count:int=int(member(modelClass,"MAX_GATES_COUNT"));
            if (count <= 0) return null;
            if (count > 32) count=32;
            var builder:Array=[];
            for (var id:int=1;id<=count;id++) {
                var vo:Object=callArgs(model,"getGateVO",[id]);
                if (vo == null) continue;
                var parts:*=member(vo,"partsCount"),totalParts:*=member(vo,"partsTotal");
                var lives:*=member(vo,"livesLeft"),lifePrice:*=member(vo,"lifePrice");
                var currentWave:*=member(vo,"currentWave"),totalWaves:*=member(vo,"totalWave");
                if (!(parts is Number) || !(totalParts is Number) || !(lives is Number) ||
                    !(lifePrice is Number) || !(currentWave is Number) || !(totalWaves is Number)) continue;
                var info:Object={id:id,
                    parts:int(parts),total_parts:int(totalParts),
                    lives:int(lives),life_price:int(lifePrice),
                    current_wave:int(currentWave),total_waves:int(totalWaves),
                    finished:member(vo,"finishState") === true,
                    multiplier:0};
                // setGate is the model's own gate-selection path; the
                // multiplier is stored per gate mode on its assets.
                callArgs(model,"setGate",[id]);
                var mult:*=callMember(model,"getMultiplierValue");
                if (mult is Number) info.multiplier=int(mult);
                builder.push(info);
            }
            if (builder.length == 0) return null;
            var data:Object={};
            var spins:*=member(modelClass,"spinamounts");
            var spinList:Array=[];
            if (spins != null) for each (var s:* in spins) if (s is Number) spinList.push(int(s));
            data.spin_amounts=spinList;
            var spinAmount:*=callMember(model,"getSpinAmount");
            if (spinAmount is Number) data.spin_amount=int(spinAmount);
            var freeSamples:*=callMember(model,"getExtraEnergy");
            if (freeSamples is Number) data.free_samples=int(freeSamples);
            var energyCost:*=callMember(model,"getEnergyCost");
            if (energyCost is Number) data.energy_cost=int(energyCost);
            var onSale:*=member(modelClass,"spinOnSale");
            if (onSale is Boolean) data.spin_on_sale=onSale;
            var gateDay:*=member(modelClass,"galaxyGateDay");
            if (gateDay is Boolean) data.galaxy_gate_day=gateDay;
            var bonusDay:*=member(modelClass,"bonusRewardsDay");
            if (bonusDay is Boolean) data.bonus_rewards_day=bonusDay;
            return {builder:builder,data:data};
        }

        // Event facts are taken only from the scoreboard widgets and data
        // proxies the game itself maintains. No current-server-time field is
        // retained by the station proxy, so it stays unreported.
        private static function eventFeatures(manager:Object,facade:Object):Object
        {
            var windows:Object=member(manager,"windows");
            var data:Object={};
            var state:Object={};
            var found:Boolean=false;
            var ball:Object=null;
            var sc:*=member(resolvePath("net.bigpoint.darkorbit.gui.container.SimpleContainer"),"CLASS_SPACEBALL");
            if (sc is Number) {
                var scores:Object={};var owner:int=0;var speed:int=-1;
                for (var cid:int=1;cid<=3;cid++) {
                    var element:DisplayObjectContainer=callArgs(manager,"getScoreElement",["spaceball",int(sc),cid]) as DisplayObjectContainer;
                    if (element == null) continue;
                    for (var c:int=0;c<element.numChildren;c++) {
                        var child:DisplayObject=element.getChildAt(c);
                        if (child is TextField && /^-?[0-9.,']+$/.test(TextField(child).text))
                            scores[String(cid)]=Number(TextField(child).text.replace(/[^0-9.-]/g,""));
                        else if (child is MovieClip && MovieClip(child).currentFrame>1) {
                            owner=cid;speed=MovieClip(child).currentFrame-1;
                        }
                    }
                }
                var anyScore:Boolean=false;
                for (var scoreKey:String in scores) { anyScore=true;break; }
                if (anyScore || speed >= 0) {
                    ball={scores:scores};
                    if (owner > 0) { ball.owner_faction=owner;state.team=owner; }
                    if (speed >= 0) ball.speed=speed;
                    found=true;
                }
            }
            var active:Array=[];
            if (ball != null) active.push("spaceball");
            for each (var name:String in ["invasion","ctb","tdm"]) {
                var eventWindow:Object=windows==null ? null : windows[name];
                if (eventWindow != null && (eventWindow.visible === true || member(eventWindow,"maximized") === true)) active.push(name);
            }
            var stations:Array=[];
            var proxy:Object=facade==null ? null : callArgs(facade,"retrieveProxy",["SpaceMapDataProxy"]);
            var list:Object=member(proxy,"spaceMapStations");
            if (list != null) for each (var st:Object in list) {
                if (st == null || stations.length >= 16) continue;
                stations.push({map_id:member(st,"mapId"),status:member(st,"status"),
                    faction:member(st,"owningFactionId"),clan:member(st,"clanName"),
                    asteroid:member(st,"asteroidName"),last_changed_unix_ms:member(st,"lastChangedTimestamp")});
            }
            if (stations.length > 0) { data.stations=stations;found=true; }
            if (ball != null) data.spaceball=ball;
            if (active.length > 0) state.active=active;
            if (!found && active.length == 0) return null;
            state.data=data;
            return state;
        }

        // Active booster categories from BoosterProxy's BoosterModel: summed
        // percentage per category and the soonest timer (seconds).
        private static function boosterFeatures(facade:Object):Array
        {
            if (facade == null) return null;
            var proxy:Object=callArgs(facade,"retrieveProxy",["BoosterProxy"]);
            if (proxy == null) return null;
            var model:Object=callMember(proxy,"getData");
            if (model == null) return null;
            var categories:*=callArgs(model,"getCategories",[false]);
            if (categories == null) return null;
            var boosters:Array=[];
            for (var i:int=0;i<categories.length && i<128;i++) {
                var category:Object=categories[i];
                var percent:*=member(category,"totalPercent");
                if (!(percent is Number) || !isFinite(Number(percent)) || Number(percent)<=0) continue;
                var entry:Object={category:String(member(category,"category")),percent:Number(percent)};
                var seconds:*=member(callMember(category,"getMinimumTimer"),"value");
                if (seconds is Number && isFinite(Number(seconds)) && Number(seconds)>0) entry.seconds_left=Number(seconds);
                boosters.push(entry);
            }
            return boosters;
        }

        private static function economyFeatures(hero:Object):Object
        {
            if (hero == null) return null;
            var econ:Object={};
            var ores:Object={};
            var patterns:Object=member(resolvePath("net.bigpoint.darkorbit.pattern.PatternManager"),"orePatterns");
            if (patterns != null) for (var oreKey:String in patterns) {
                var pattern:Object=patterns[oreKey];
                var count:*=member(pattern,"count");
                if (count is Number && isFinite(Number(count)) && Number(count)>=0)
                    ores[oreKey]=Number(count);
            }
            econ.ores=ores;
            putNumber(econ,"cargo",nestedValue(hero,"cargo","value"));
            putNumber(econ,"max_cargo",nestedValue(hero,"maxCargo","value"));
            putNumber(econ,"credits",member(hero,"creditsAmount"));
            putNumber(econ,"uridium",member(hero,"uridiumAmount"));
            var trade:*=member(hero,"tradeAreaType");
            if (trade is Number) {
                econ.trade_available=int(trade)!=0;
                econ.data={trade_area_type:int(trade)};
            }
            return econ;
        }

        private static function callArgs(target:Object,method:String,args:Array):*
        {
            var callable:*=member(target,method);
            if (callable is Function)
            {
                try
                {
                    return (callable as Function).apply(target,args);
                }
                catch (error:Error)
                {
                }
            }
            return null;
        }

        // Hooks the public log-message notification so the journal sees every
        // entry the LogWindowMediator buffer receives, in order, without
        // touching the mediator's protected buffer. Retried until the game's
        // view/observer classes exist.
        private static function ensureLogHook():void
        {
            if (_logHooked) return;
            var facade:Object=resolvePath("net.bigpoint.darkorbit.mvc.application.GameFacade.getInstance()");
            var viewClass:Object=resolvePath("org.puremvc.as3.multicore.core.View");
            var observerClass:Class=resolvePath("org.puremvc.as3.multicore.patterns.observer.Observer") as Class;
            var name:*=member(resolvePath("net.bigpoint.darkorbit.mvc.gui.GuiNotifications"),"LOG_MESSAGE");
            if (facade == null || viewClass == null || observerClass == null || !(name is String)) return;
            var key:String="Game";
            var keyValue:*=member(resolvePath("net.bigpoint.darkorbit.mvc.application.GameFacade"),"MULTITON_KEY");
            if (keyValue is String && String(keyValue)!="") key=String(keyValue);
            var view:Object=callArgs(viewClass,"getInstance",[key]);
            if (view == null) return;
            _logObserver=new observerClass(onLogNote,null);
            view.registerObserver(name,_logObserver);
            _logHooked=true;
        }

        private static function onLogNote(note:Object):void
        {
            try
            {
                var params:Object=callMember(note,"getBody");
                var text:*=member(params,"message");
                if (!(text is String)) return;
                var hero:Object=resolvePath("net.bigpoint.darkorbit.Hero");
                pushEvent("game_log",String(text).replace(/^\s+|\s+$/g,""),
                    {user_id:member(hero,"userID"),type:member(params,"type"),
                     id:member(params,"id"),rewards:templateRewards(String(text))});
            }
            catch (error:Error)
            {
            }
        }

        private static function pushEvent(kind:String,text:String,data:Object):void
        {
            _eventSeq++;
            _events.push({sequence:_eventSeq,observed_at_unix_ms:new Date().time,kind:kind,text:text,data:data});
            while (_events.length > 4096) _events.shift();
        }

        // The localized reward sentences wrap the amount in a %! wildcard.
        // Only their numeric payload is reported, never deductions.
        private static function templateRewards(text:String):*
        {
            var locale:Object=resolvePath("net.bigpoint.as3toolbox.bplocale.BPLocale");
            if (!_templatesDone && locale != null)
            {
                var keys:Array=["getcredits","geturidium","gatherep","getexp"];
                var loaded:int=0;
                for (var i:int=0;i<4;i++)
                {
                    if (_rewardTemplates[i] != null) { loaded++;continue; }
                    var template:*=callArgs(locale,"getItem",[keys[i]]);
                    if (template is String)
                    {
                        var cut:int=String(template).indexOf("%!");
                        if (cut >= 0)
                        {
                            _rewardTemplates[i]=[String(template).substr(0,cut),String(template).substr(cut+2)];
                            loaded++;
                        }
                    }
                }
                _templatesDone=loaded >= 4;
            }
            for (var j:int=0;j<4;j++)
            {
                var pair:Array=_rewardTemplates[j] as Array;
                if (pair == null) continue;
                var head:int=String(pair[0]).length,tail:int=String(pair[1]).length;
                if (text.length < head+tail) continue;
                if (text.substr(0,head) != pair[0]) continue;
                if (tail > 0 && text.substr(text.length-tail) != pair[1]) continue;
                var mid:String=text.substring(head,text.length-tail).replace(/^\s+|\s+$/g,"");
                if (mid == "" || mid.charAt(0) == "-") continue;
                if (!/^[0-9\s.,']+$/.test(mid)) continue;
                var amount:Number=Number(mid.replace(/[^0-9]/g,""));
                if (isFinite(amount))
                {
                    var rewards:Array=[0,0,0,0];
                    rewards[j]=amount;
                    return rewards;
                }
            }
            return null;
        }

        // Bounded cursor replay: ordered events, monotonic next_sequence and a
        // dropped flag when the cursor predates the oldest retained event.
        private static function clientEvents(after:*):Object
        {
            ensureLogHook();
            var a:Number=after==null ? 0 : Number(after);
            if (!isFinite(a) || a<0 || a!=Math.floor(a)) throw new Error("Invalid event cursor");
            if (a>_eventSeq) throw new Error("event cursor is ahead of this client session");
            var out:Array=[];
            for each (var e:Object in _events) if (Number(e.sequence)>a) out.push(e);
            return {events:out,next_sequence:_eventSeq,
                dropped:_events.length>0 && a<Number(_events[0].sequence)-1};
        }

        private static function hasClickTarget(d:DisplayObject):Boolean
        {
            return d != null && (d.hasEventListener(MouseEvent.CLICK) || d.hasEventListener("click"));
        }

        private static function appendGuiTree(d:DisplayObject,windowId:String,path:String,out:Array,stageRoot:DisplayObjectContainer):void
        {
            if (d==null || !d.visible || d.alpha<=0 || out.length>=256) return;
            var rootWindow:Boolean=path=="";
            if (rootWindow || hasClickTarget(d)) {
                var b:Rectangle;
                try { b=d.getBounds(stageRoot); } catch (error:Error) { b=null; }
                if (b!=null) {
                    var label:String=null;
                    if (d is TextField) label=TextField(d).text;
                    else for each (var child:DisplayObject in displayNodes(d as DisplayObjectContainer,32))
                        if (child is TextField && TextField(child).text!="") { label=TextField(child).text; break; }
                    out.push({id:rootWindow ? windowId : windowId+"/"+path,visible:true,
                        bounds:{x:b.x,y:b.y,width:b.width,height:b.height},text:label,enabled:member(d,"enabled")});
                }
            }
            var container:DisplayObjectContainer=d as DisplayObjectContainer;
            if (container==null) return;
            for (var i:int=0;i<container.numChildren && out.length<256;i++)
                appendGuiTree(container.getChildAt(i),windowId,path=="" ? String(i) : path+"/"+i,out,stageRoot);
        }

        private static function findGuiById(id:String, clickOnly:Boolean=false):DisplayObject
        {
            var manager:Object=resolvePath("net.bigpoint.darkorbit.gui.GuiManager.instance");
            var windows:Object=member(manager,"windows");
            var stageRoot:DisplayObjectContainer=_clip == null ? null : _clip.stage;
            if (windows==null) return null;
            for (var key:String in windows) {
                var window:DisplayObject=windows[key] as DisplayObject;
                if (window==null || !window.visible || window.alpha<=0) continue;
                if (id==key && !clickOnly) return window;
                var indexed:Array=[];
                appendGuiTree(window,key,"",indexed,stageRoot);
                for each (var element:Object in indexed) if (element.id==id) {
                    if (id==key || !clickOnly || hasClickTarget(resolveGuiPath(window,id,key))) return resolveGuiPath(window,id,key);
                }
            }
            return null;
        }

        private static function resolveGuiPath(window:DisplayObject,id:String,windowId:String):DisplayObject
        {
            if (id==windowId) return window;
            var suffix:String=id.substr(windowId.length+1);
            var current:DisplayObject=window;
            for each (var part:String in suffix.split("/")) {
                var container:DisplayObjectContainer=current as DisplayObjectContainer;
                var index:int=int(part);
                if (container==null || index<0 || index>=container.numChildren) return null;
                current=container.getChildAt(index);
            }
            return current;
        }

        private static function sendOreRequest(command:Object, facade:Object):void
        {
            var ore:int=int(command.ore_type), amount:Number=Number(command.amount);
            if ((ore<0 || ore>8 || ore==3) && ore!=28 || amount<=0 || !isFinite(amount) || amount>9007199254740991 || Math.floor(amount)!=amount)
                throw new Error("Invalid ore type or amount");
            var type:Class=resolvePath("net.bigpoint.darkorbit.com.module.OreTypeModule") as Class;
            var count:Class=resolvePath("net.bigpoint.darkorbit.com.module.OreCountModule") as Class;
            if (type==null || count==null) throw new Error("Ore request classes unavailable");
            var oreType:Object=new type(ore);
            var oreCount:Object=new count(oreType,amount);
            var requestName:String=command.command=="sell_ore" ? "TradeSellOreRequest" : "LabRefinementRequest";
            if (command.command=="sell_ore") {
                if (ore==3) throw new Error("Xenomit cannot be sold");
                var screen:Object=resolvePath("net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
                var layer:DisplayObjectContainer=callMember(screen,"getWindowLayer2") as DisplayObjectContainer;
                var tradeVisible:Boolean=false;
                if (layer!=null) for each (var d:DisplayObject in displayNodes(layer))
                    if (getQualifiedClassName(d).indexOf("SimpleWindow")>=0 && member(d,"id")=="ore_trade" && d.visible) tradeVisible=true;
                if (!tradeVisible) throw new Error("Ore sale requires the visible trade window");
            } else {
                var gameXml:*=resolvePath("net.bigpoint.darkorbit.Main").gameXML;
                var outputs:*=gameXml==null ? null : gameXml.refinementWindow.refinement.ores.ore;
                var configured:Boolean=false;
                if (outputs!=null) for each (var output:XML in outputs)
                    if (int(output.@type)==ore && String(output.@button).toLowerCase()=="true") configured=true;
                if (!configured) throw new Error("Ore type is not a configured refinement output");
            }
            var req:Object=request(requestName,[oreCount]);
            if (command.command=="refine_ore") req.toProduce=oreCount; else req.toSell=oreCount;
            facade.sendNotification("SEND_NETTY_REQUEST",req);
        }

        private static function setOverlay(command:Object):void
        {
            var id:String=String(command.id), value:String=String(command.text);
            var x:Number=Number(command.x), y:Number=Number(command.y), color:uint=uint(command.color);
            if (id=="" || id.length>128 || value.length>2048 || !isFinite(x) || !isFinite(y))
                throw new Error("Invalid overlay id, text, or position");
            if (_clip==null || _clip.stage==null) throw new Error("Game stage unavailable");
            if (_overlayLayer==null) {
                _overlayLayer=new Sprite();
                _overlayLayer.mouseEnabled=false;
                _overlayLayer.mouseChildren=false;
                _clip.stage.addChild(_overlayLayer);
            }
            var field:TextField=_overlays[id] as TextField;
            if (field==null) {
                field=new TextField();
                field.mouseEnabled=false;
                field.mouseWheelEnabled=false;
                field.selectable=false;
                field.autoSize="left";
                _overlays[id]=field;
                _overlayLayer.addChild(field);
            }
            field.text=value;
            field.defaultTextFormat=new TextFormat("Arial",14,color,true);
            field.setTextFormat(field.defaultTextFormat);
            field.x=x;
            field.y=y;
        }

        private static function removeOverlay(id:String):void
        {
            var field:TextField=_overlays[id] as TextField;
            if (field==null) throw new Error("Overlay id is not owned by the bridge");
            if (field.parent==_overlayLayer) _overlayLayer.removeChild(field);
            delete _overlays[id];
            if (_overlayLayer!=null && _overlayLayer.numChildren==0 && _overlayLayer.parent!=null) {
                _overlayLayer.parent.removeChild(_overlayLayer);
                _overlayLayer=null;
            }
        }
        private static function freeRepair():Object
        {
            var hero:Object=resolvePath("net.bigpoint.darkorbit.Hero");
            if (member(hero,"isKilled") !== true) return null;
            var screen:Object=resolvePath("net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
            var layer:DisplayObjectContainer=callMember(screen,"getWindowLayer2") as DisplayObjectContainer;
            var locale:Object=resolvePath("net.bigpoint.as3toolbox.bplocale.BPLocale");
            if (locale == null) return null;
            var free:String=String(locale.getItem("pricetag_killscreen_free"));
            if (free=="" || free=="pricetag_killscreen_free") return null;
            for each (var d:DisplayObject in displayNodes(layer)) {
                if (getQualifiedClassName(d)!="net.bigpoint.darkorbit.gui.windows.components.repair::KillScreenWindow" || !d.visible || d.alpha<1) continue;
                for each (var child:DisplayObject in displayNodes(d as DisplayObjectContainer)) {
                    if (getQualifiedClassName(child)!="net.bigpoint.darkorbit.gui.elements::ButtonElement") continue;
                    var button:Object=child;
                    var type:int=int(button.getType());
                    if ((type!=1 && type!=6) || !button.hasEventListener(MouseEvent.CLICK) || button.getChildByName("mask")!=null) continue;
                    var labelFound:Boolean=false;
                    for each (var field:DisplayObject in displayNodes(child as DisplayObjectContainer,64))
                        if (field is TextField && TextField(field).text==free) labelFound=true;
                    if (labelFound) return {type:type,window:d,button:button};
                }
            }
            return null;
        }
        private static function request(name:String,args:Array):Object
        {
            var type:Class = resolvePath("net.bigpoint.darkorbit.com.module."+name) as Class;
            if (type == null) throw new Error("Request class unavailable: "+name);
            switch(args.length) {
                case 0:return new type();
                case 1:return new type(args[0]);
                case 2:return new type(args[0],args[1]);
                case 3:return new type(args[0],args[1],args[2]);
            }
            throw new Error("Unsupported request arity");
        }
        private static function gameCommand(command:Object):Object
        {
            var facade:Object=resolvePath("net.bigpoint.darkorbit.mvc.application.GameFacade.getInstance()");
            var screen:Object=resolvePath("net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
            var map:Object=member(screen,"map");
            var hero:Object=resolvePath("net.bigpoint.darkorbit.Hero");
            var ship:Object=member(map,"hero");
            if (facade == null || (ship == null && command.command!="revive")) throw new Error("Game not ready");
            var req:Object;
            switch(command.command) {
                case "click_gui":
                    var gui:DisplayObject=findGuiById(String(command.name),true);
                    if (gui==null) throw new Error("GUI element is no longer visible or clickable");
                    gui.dispatchEvent(new MouseEvent(MouseEvent.CLICK,true,true)); return null;
                case "close_gui":
                    var guiManager:Object=resolvePath("net.bigpoint.darkorbit.gui.GuiManager.instance");
                    var guiWindows:Object=member(guiManager,"windows");
                    var closeTarget:Object=guiWindows==null ? null : guiWindows[String(command.name)];
                    if (closeTarget==null || !closeTarget.visible) throw new Error("GUI window is no longer open");
                    guiManager.closeWindow(closeTarget,true); return null;
                case "set_overlay":
                    setOverlay(command); return null;
                case "remove_overlay":
                    removeOverlay(String(command.id)); return null;
                case "sell_ore":
                case "refine_ore":
                    sendOreRequest(command,facade); return null;
                case "jump_portal":
                    var portal:Object=map.getEntityById(int(command.portal_id));
                    if (portal == null || getQualifiedClassName(portal).indexOf("Portal") < 0) throw new Error("Portal disappeared");
                    var dx:Number=portal.position.x-ship.position.x,dy:Number=portal.position.y-ship.position.y;
                    if (dx*dx+dy*dy > 600*600) throw new Error("Hero too far from portal");
                    if (member(portal,"isJumping") === true) return null;
                    req=request("JumpRequest",[]); break;
                case "set_pet":
                    if (hero.ownsPet !== true || hero.isPetDestroyed === true) throw new Error("Pet unavailable or destroyed");
                    var active:Boolean=member(ship,"pet") != null && member(ship.pet,"container") != null;
                    if (active == Boolean(command.enabled)) return null;
                    req=request("PetRequest",[int(command.enabled ? 0 : 1)]); break;
                case "select_configuration":
                    var configuration:int=int(command.configuration);
                    if (configuration < 1 || configuration > 2) throw new Error("Configuration must be 1 or 2");
                    var settings:Object=resolvePath("net.bigpoint.darkorbit.settings.Settings");
                    if (settings.selectedConfiguration != configuration)
                        facade.sendNotification("ConnectionNotificationSendCommand",["S",["CFG",String(configuration),String(hero.userID),String(hero.sessionID)]]);
                    return null;
                case "use_item":
                    var proxy:Object=facade.retrieveProxy("ItemsControlMenuProxy");
                    var item:Object=proxy.getItemVOByItemId(String(command.item_id));
                    if (item == null || item.available !== true || item.blocked !== false || (command.activate && item.activatable !== true)) throw new Error("Item unavailable, blocked, or on cooldown");
                    if (!command.activate && item.selected === true) return null;
                    proxy.sendActionRequest(request("MenuActionRequest",[String(command.item_id),int(command.activate ? 1 : 0),int(0)])); return null;
                case "select_pet_gear":
                    if (member(ship,"pet")==null || member(ship.pet,"container")==null) throw new Error("Pet not active");
                    var menu:Object=petGearMenu();
                    if (menu==null || menu.getElementByID(int(command.gear_type))==null) throw new Error("Pet gear not equipped");
                    req=request("PetGearActivationRequest",[request("PetGearTypeModule",[int(command.gear_type)]),int(command.target)]);break;
                case "revive":
                    var offer:Object=freeRepair();
                    if (offer==null || offer.type!=int(command.repair_type)) throw new Error("Automatic revive requires an offered, cooldown-free, free base repair");
                    offer.button.dispatchEvent(new MouseEvent(MouseEvent.CLICK));
                    var repairEvent:Class=resolvePath("net.bigpoint.darkorbit.gui.windows.components.repair.RepairEvent") as Class;
                    if (repairEvent==null) throw new Error("Repair event unavailable");
                    offer.window.dispatchEvent(new repairEvent("REPAIR"));return null;
                case "revive_at":
                    if (String(command.location)!="base") throw new Error("This client only automates an offered free base repair");
                    var baseOffer:Object=freeRepair();
                    if (baseOffer==null || baseOffer.type!=int(command.repair_type)) throw new Error("Automatic revive requires an offered, cooldown-free, free base repair");
                    baseOffer.button.dispatchEvent(new MouseEvent(MouseEvent.CLICK));
                    var baseRepairEvent:Class=resolvePath("net.bigpoint.darkorbit.gui.windows.components.repair.RepairEvent") as Class;
                    if (baseRepairEvent==null) throw new Error("Repair event unavailable");
                    baseOffer.window.dispatchEvent(new baseRepairEvent("REPAIR"));return null;
                case "repair_drones":
                case "invite_group_member":
                case "leave_group":
                case "kick_group_member":
                case "set_group_invites":
                    throw new Error("This manager operation is unavailable in the Flash bridge");
                case "select_gate_difficulty":
                    selectGateDifficulty(command,resolvePath("net.bigpoint.darkorbit.gui.GuiManager.instance")); return null;
                case "spin_galaxy_gate":
                    var spinGate:int=validatedGate(command.gate_id);
                    var spinAmount:int=int(command.spin_amount);
                    if (spinAmount!=1 && spinAmount!=5 && spinAmount!=10 && spinAmount!=100) throw new Error("Galaxy spin amount must be 1, 5, 10, or 100");
                    gateVO(ggBuilderModel(facade),spinGate);
                    req=request("GalaxyGateBuilderMultiEnergyRequest",[spinGate,Boolean(command.use_multiplier),spinAmount]); break;
                case "place_galaxy_gate":
                    var placeGate:int=validatedGate(command.gate_id);
                    var placeVO:Object=gateVO(ggBuilderModel(facade),placeGate);
                    if (member(placeVO,"finishState")!==true) throw new Error("Galaxy gate is not complete");
                    req=request("GalaxyGateBuilderSetupGateRequest",[placeGate]); break;
                case "buy_galaxy_life":
                    var lifeGate:int=validatedGate(command.gate_id);
                    var lifeVO:Object=gateVO(ggBuilderModel(facade),lifeGate);
                    var lifePrice:*=member(lifeVO,"lifePrice");
                    if (!(lifePrice is Number) || int(lifePrice)<0) throw new Error("Galaxy gate offers no purchasable life");
                    req=request("GalaxyGateBuilderBuyLifeRequest",[lifeGate]); break;
                case "accept_group_invite":
                    var inviter:int=int(command.inviter_id);
                    if (inviter<=0) throw new Error("Group inviter id is missing");
                    req=request("InvitationAcceptRequest",[inviter]); break;
                default:throw new Error("Unknown game command");
            }
            facade.sendNotification("SEND_NETTY_REQUEST",req);
            return null;
        }
        private static function validatedGate(value:*):int
        {
            var gateId:int=int(value);
            if (!(value is Number) || Number(value)!=gateId || !((gateId>=1 && gateId<=8) || gateId==12 || gateId==19))
                throw new Error("Unknown galaxy gate id");
            return gateId;
        }
        // The builder model is required for gate commands so paid actions are
        // only issued against observed, server-provided gate state.
        private static function gateVO(model:Object,gateId:int):Object
        {
            if (model == null) throw new Error("Galaxy gate builder is unavailable");
            var vo:Object=callArgs(model,"getGateVO",[gateId]);
            if (vo == null) throw new Error("Galaxy gate is not initialized");
            return vo;
        }
        // Only a difficulty actually offered by the observed selection window's
        // dropdown may be chosen; the window's own go button then sends the
        // real enter request.
        private static function selectGateDifficulty(command:Object,manager:Object):void
        {
            var gui:String=String(command.gui);
            var difficulty:int=int(command.difficulty);
            if (!(command.difficulty is Number) || Number(command.difficulty)!=difficulty || difficulty<=0)
                throw new Error("Gate difficulty must be a positive integer");
            var windows:Object=member(manager,"windows");
            var windowKey:String=gui.split("/")[0];
            var window:DisplayObject=windows==null ? null : windows[windowKey] as DisplayObject;
            if (window==null || !window.visible) throw new Error("Difficulty selection window is no longer open");
            var dropdowns:Array=[];
            var goButton:DisplayObject=null;
            for each (var d:DisplayObject in displayNodes(window as DisplayObjectContainer)) {
                if (getQualifiedClassName(d)=="net.bigpoint.darkorbit.fui.components.list.dropdownbox::DropDownBox") dropdowns.push(d);
                else if (d.name=="button_go" && goButton==null) goButton=d;
            }
            var chosen:DisplayObject=null;
            var entryIndex:int=-1;
            for each (var box:DisplayObject in dropdowns) {
                var original:int=int(member(box,"currentEntry"));
                var sawDifficulty:Boolean=false;
                for (var i:int=0;i<64;i++) {
                    try { box["currentEntry"]=i; } catch (setError:Error) { break; }
                    if (int(member(box,"currentEntry"))<i) break;
                    var key:*=member(box,"currentEntryKey");
                    if (!(key is String) || String(key)=="") break;
                    if (String(key).indexOf("difficulty")<0) break;
                    sawDifficulty=true;
                    var parsed:Array=String(key).match(/difficulty_?(\d+)$/);
                    if (parsed!=null && int(parsed[1])==difficulty) { chosen=box;entryIndex=i;break; }
                }
                if (chosen!=null) break;
                try { box["currentEntry"]=original; } catch (restoreError:Error) {}
                if (!sawDifficulty) continue;
            }
            if (chosen==null) throw new Error("Requested difficulty is not offered by the observed selection UI");
            try { chosen["currentEntry"]=entryIndex; } catch (selectError:Error) { throw new Error("Difficulty selection was rejected by the window"); }
            var selectedKey:*=member(chosen,"currentEntryKey");
            var verify:Array=selectedKey is String ? String(selectedKey).match(/difficulty_?(\d+)$/) : null;
            if (verify==null || int(verify[1])!=difficulty || int(member(chosen,"currentEntry"))!=entryIndex)
                throw new Error("Difficulty selection was rejected by the window");
            if (goButton==null) throw new Error("Difficulty selection go button unavailable");
            if (member(goButton,"enabled")===false || member(goButton,"enabled")===0)
                throw new Error("Selected difficulty is not currently enterable");
            goButton.dispatchEvent(new MouseEvent(MouseEvent.CLICK,true,true));
        }
        private static function displayRoot():DisplayObject
        {
            if (_clip != null)
            {
                if (_clip.root != null)
                {
                    return _clip.root;
                }
                return _clip;
            }
            try
            {
                var info:LoaderInfo = LoaderInfo.getLoaderInfoByDefinition(DobotBridge);
                return info != null ? info.content as DisplayObject : null;
            }
            catch (error:Error)
            {
                return null;
            }
            return null;
        }

        private static function collectDomains():Array
        {
            if (getTimer() - _domainAt < 1000) return _domains;
            _domainAt = getTimer();
            var domains:Array = [];
            var seen:Dictionary = new Dictionary(true);
            var root:DisplayObject = displayRoot();
            if (root != null)
            {
                if (root.stage != null)
                {
                    walk(root.stage, domains, seen, 0);
                }
                walk(root, domains, seen, 0);
            }
            _domains = domains;
            return domains;
        }

        private static function walk(
            object:DisplayObject, domains:Array, seen:Dictionary, depth:int):void
        {
            if (object == null || depth > 48 || seen[object])
            {
                return;
            }
            seen[object] = true;
            // Loaded game content sits on the preloader as a normal child, not only
            // as a Loader. Its LoaderInfo carries the SWF's ApplicationDomain.
            try
            {
                var info:LoaderInfo = object.loaderInfo;
                if (info != null)
                {
                    var objectDomain:ApplicationDomain = info.applicationDomain;
                    if (objectDomain != null && domains.indexOf(objectDomain) < 0)
                    {
                        domains.push(objectDomain);
                    }
                }
            }
            catch (error:Error)
            {
            }
            if (object is Loader)
            {
                var loader:Loader = Loader(object);
                try
                {
                    var domain:ApplicationDomain = loader.contentLoaderInfo.applicationDomain;
                    if (domain != null && domains.indexOf(domain) < 0)
                    {
                        domains.push(domain);
                    }
                }
                catch (error:Error)
                {
                }
                try
                {
                    if (loader.content != null)
                    {
                        walk(loader.content, domains, seen, depth + 1);
                    }
                }
                catch (error:Error)
                {
                }
            }
            var container:DisplayObjectContainer = object as DisplayObjectContainer;
            if (container == null)
            {
                return;
            }
            var count:int = container.numChildren;
            for (var index:int = 0; index < count; index++)
            {
                try
                {
                    walk(container.getChildAt(index), domains, seen, depth + 1);
                }
                catch (error:Error)
                {
                }
            }
        }

        private static function findDefinition(name:String, domains:Array):*
        {
            if (_classes[name] != null) return _classes[name];
            try
            {
                return _classes[name] = getDefinitionByName(name);
            }
            catch (error:Error)
            {
            }
            var index:int;
            for (index = 0; index < domains.length; index++)
            {
                var domain:ApplicationDomain = domains[index] as ApplicationDomain;
                try
                {
                    if (domain != null && domain.hasDefinition(name))
                    {
                        return _classes[name] = domain.getDefinition(name);
                    }
                }
                catch (error:Error)
                {
                }
            }
            return null;
        }

        private static function step(current:Object, part:String):*
        {
            if (current == null)
            {
                return null;
            }
            var invokeNow:Boolean = false;
            var name:String = part;
            if (name.length >= 2 && name.substring(name.length - 2) == "()")
            {
                invokeNow = true;
                name = name.substring(0, name.length - 2);
            }
            var next:* = null;
            try
            {
                next = name in current ? current[name] : null;
            }
            catch (error:Error)
            {
                return null;
            }
            if (!invokeNow)
            {
                return next;
            }
            if (!(next is Function))
            {
                throw new Error("not a function: " + name);
            }
            return (next as Function).apply(current, []);
        }

        /**
         * Resolve a class, a static/property chain, or a display-list chain.
         * A segment ending in () is called with no arguments, which is how
         * singletons such as ScreenManager.getInstance() are reached.
         */
        private static function resolvePath(path:String):Object
        {
            if (path == null || path == "" || path == "root")
            {
                return displayRoot();
            }
            if (path.indexOf("map/")==0) {
                var idText:String=path.substr(4);
                if (!/^-?[0-9]+$/.test(idText)) return null;
                return resolveEntity(Number(idText));
            }
            if (path == "stage")
            {
                if (_clip != null && _clip.stage != null)
                {
                    return _clip.stage;
                }
                var root:DisplayObject = displayRoot();
                return root != null ? root.stage : null;
            }

            var binding:Object = _bindings[path];
            var parts:Array;
            if (binding != null) {
                var cached:Object = binding.defined;
                parts=binding.parts;
                for (var c:int=binding.length;c<parts.length;c++) cached=step(cached,String(parts[c]));
                return cached;
            }
            parts=path.split(".");
            var domains:Array = collectDomains();
            var classLength:int;
            for (classLength = parts.length; classLength >= 1; classLength--)
            {
                var className:String = parts.slice(0, classLength).join(".");
                var defined:* = findDefinition(className, domains);
                if (defined == null)
                {
                    continue;
                }
                var current:Object = defined;
                var index:int;
                for (index = classLength; index < parts.length; index++)
                {
                    current = step(current, String(parts[index]));
                    if (current == null)
                    {
                        break;
                    }
                }
                if (index == parts.length)
                {
                    _bindings[path]={defined:defined,length:classLength,parts:parts};
                    return current;
                }
            }

            var display:DisplayObject = displayRoot();
            var cursor:Object = display;
            var start:int = 0;
            if (parts.length > 0 && display is DisplayObjectContainer)
            {
                var container:DisplayObjectContainer = DisplayObjectContainer(display);
                var child:DisplayObject = container.getChildByName(String(parts[0]));
                if (child == null && display.stage != null)
                {
                    child = display.stage.getChildByName(String(parts[0]));
                }
                if (child != null)
                {
                    cursor = child;
                    start = 1;
                }
            }
            for (var partIndex:int = start; partIndex < parts.length; partIndex++)
            {
                cursor = step(cursor, String(parts[partIndex]));
                if (cursor == null)
                {
                    return null;
                }
            }
            return cursor;
        }

        private static function resolveEntity(id:Number):Object {
            if (isNaN(id) || id!=Math.floor(id) || id < -2147483648 || id > 2147483647) throw new Error("invalid entity ID");
            var screen:Object=resolvePath("net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
            var map:Object=member(screen,"map");
            if (map==null) throw new Error("game map unavailable");
            var entity:Object=map.getEntityById(int(id));
            if (entity==null) throw new Error("entity no longer exists");
            return entity;
        }
        private static function coerce(value:*):*
        {
            if (value is Array)
            {
                var copy:Array = [];
                var index:int;
                for (index = 0; index < value.length; index++)
                {
                    copy.push(coerce(value[index]));
                }
                return copy;
            }
            if (value != null && value is Object &&
                !(value is String) && !(value is Number) && !(value is Boolean))
            {
                // The controller refers to a live map object by the stable ID
                // included in model snapshots. Resolve it here so calls such
                // as moveHeroToCordinates(x, y, collectable) receive the real
                // ActionScript Collectable instance, not a JSON-shaped copy.
                if (value.hasOwnProperty("$xenomit_entity_id"))
                {
                    return resolveEntity(Number(value["$xenomit_entity_id"]));
                }
                var object:Object = {};
                for (var key:String in value)
                {
                    object[key] = coerce(value[key]);
                }
                return object;
            }
            if (value is Number)
            {
                var number:Number = Number(value);
                if (!isNaN(number) && number == Math.floor(number) &&
                    number >= -2147483648 && number <= 2147483647)
                {
                    return int(number);
                }
            }
            return value;
        }

        private static function fail(message:String, stack:String = ""):String
        {
            var payload:Object = {status: "error", message: message};
            if (stack != null && stack != "")
            {
                payload.stack = stack;
            }
            return JSON.stringify(payload);
        }

        private static function succeed(result:*):String
        {
            var data:* = result === undefined ? null : result;
            try
            {
                return JSON.stringify({status: "ok", data: data});
            }
            catch (error:Error)
            {
                return JSON.stringify({
                    status: "ok",
                    data: String(result),
                    note: "result was not JSON-serializable"
                });
            }
            return fail("result was not JSON-serializable");
        }

        public static function invoke(targetPath:String, method:String, jsonArgs:String):String
        {
            try
            {
                if (targetPath == "xenomit" && method == "gameCommand") return succeed(gameCommand(JSON.parse(jsonArgs)[0]));
                if (targetPath == "xenomit" && method == "clientFeatures") return succeed(clientFeatures());
                if (targetPath == "xenomit" && method == "direct")
                {
                    var directArgs:* = JSON.parse(jsonArgs);
                    if (!(directArgs is Array)) return fail("arguments must be a JSON array");
                    return succeed(direct(coerce(directArgs) as Array));
                }
                if (targetPath == "xenomit" && method == "caps") return succeed(caps());
                if (targetPath == "xenomit" && method == "clientEvents")
                {
                    var eventArgs:*=JSON.parse(jsonArgs);
                    return succeed(clientEvents(eventArgs is Array && eventArgs.length>0 ? eventArgs[0] : 0));
                }
                var args:Array = [];
                if (jsonArgs != null && jsonArgs != "")
                {
                    var parsed:* = JSON.parse(jsonArgs);
                    if (!(parsed is Array))
                    {
                        return fail("arguments must be a JSON array");
                    }
                    args = coerce(parsed) as Array;
                }
                var target:Object = resolvePath(targetPath);
                if (target == null || !(target[method] is Function))
                {
                    return fail("Method or target not found");
                }
                var result:* = (target[method] as Function).apply(target, args);
                return succeed(result);
            }
            catch (error:Error)
            {
                var stack:String = error.getStackTrace();
                return fail(error.message, stack == null ? "" : stack);
            }
            return fail("invoke failed");
        }

        public static function inspect(targetPath:String):String
        {
            try
            {
                var target:Object = resolvePath(targetPath);
                if (target == null)
                {
                    return fail("target not found");
                }
                var xml:XML = describeType(target);
                var methods:Array = [];
                var accessors:Array = [];
                var variables:Array = [];
                collectMembers(xml.method, "declared", methods, accessors, variables, true);
                collectMembers(xml.accessor, "declared", methods, accessors, variables, false);
                collectMembers(xml.variable, "declared", methods, accessors, variables, false);
                if (xml.factory.length() > 0)
                {
                    var factory:XML = xml.factory[0];
                    collectMembers(factory.method, "instance", methods, accessors, variables, true);
                    collectMembers(factory.accessor, "instance", methods, accessors, variables, false);
                    collectMembers(factory.variable, "instance", methods, accessors, variables, false);
                }
                return JSON.stringify({
                    status: "ok",
                    name: String(xml.@name),
                    isClass: target is Class,
                    qualified: getQualifiedClassName(target),
                    methods: methods,
                    accessors: accessors,
                    variables: variables
                });
            }
            catch (error:Error)
            {
                var stack:String = error.getStackTrace();
                return fail(error.message, stack == null ? "" : stack);
            }
            return fail("inspect failed");
        }

        /** A snapshot of displayed objects, taken on the player thread. */
        public static function state(targetPath:String):String
        {
            try
            {
                var target:Object = resolvePath(targetPath);
                var root:DisplayObject = target as DisplayObject;
                if (root == null)
                {
                    return fail("target is not a display object");
                }
                var stats:Object = {visited: 0, truncated: false};
                // The map model has stable game entity IDs. Use it directly
                // and reserve the more expensive display-list walk for screens
                // where the map model is not available yet.
                var result:Array = mapEntitiesSnapshot();
                if (result == null)
                {
                    result = [];
                    snapshot(root, result, stats, "root", 0);
                }
                else
                {
                    stats.visited = result.length;
                }
                return JSON.stringify({status: "ok", data: {
                    stageWidth: root.stage == null ? null : root.stage.stageWidth,
                    stageHeight: root.stage == null ? null : root.stage.stageHeight,
                    map: mapSnapshot(),
                    entities: result,
                    visited: stats.visited,
                    truncated: stats.truncated
                }});
            }
            catch (error:Error)
            {
                return fail(error.message, error.getStackTrace());
            }
            return fail("state failed");
        }

        /**
         * Prefer actual model objects while the game is on a map. Display-list
         * wrappers do not carry the stable IDs needed by behavior decisions.
         * Keep x/y in stage pixels for the shared API and provide worldX/Y for
         * the runtime planner's movement and distance calculations.
         */
        private static function mapEntitiesSnapshot(compact:Boolean=false):Array
        {
            try
            {
                var screen:Object = resolvePath(
                    "net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
                var gameMap:Object = member(screen, "map");
                var mapView:Object = member(screen, "mapView");
                var view:DisplayObject = mapView as DisplayObject;
                if (gameMap == null || member(gameMap, "hero") == null || view == null)
                {
                    return null;
                }
                var source:* = member(gameMap, "entities");
                if (source == null)
                {
                    return null;
                }

                var result:Array = [];
                for each (var entity:Object in source)
                {
                    if (entity == null)
                    {
                        continue;
                    }
                    var id:* = member(entity, "id");
                    var position:Object = member(entity, "position");
                    if (id == null || position == null)
                    {
                        continue;
                    }
                    var worldX:Number = Number(member(position, "x"));
                    var worldY:Number = Number(member(position, "y"));
                    if (isNaN(worldX) || isNaN(worldY))
                    {
                        continue;
                    }

                    var stageX:Number = worldX;
                    var stageY:Number = worldY;
                    try
                    {
                        var screenPoint:Object = mapView.game2screen(int(worldX), int(worldY));
                        if (screenPoint != null)
                        {
                            var globalPoint:Point = view.localToGlobal(
                                new Point(Number(screenPoint.x), Number(screenPoint.y)));
                            stageX = globalPoint.x;
                            stageY = globalPoint.y;
                        }
                    }
                    catch (screenError:Error)
                    {
                    }

                    var className:String = getQualifiedClassName(entity);
                    var fields:Object = {id: id, worldX: worldX, worldY: worldY};
                    var npc:* = member(entity, "isNPC");
                    if (npc != null)
                    {
                        fields.isNpc = Boolean(npc);
                    }
                    if (npc != null)
                    {
                        fields.isHero = callBoolean(entity, "isHeroShip");
                        var hpTrait:Object = member(entity, "hp");
                        if (fields.isHero) {
                            var heroClass:Object = resolvePath("net.bigpoint.darkorbit.Hero");
                            putNumber(fields,"cargo", nestedValue(heroClass,"cargo","value"));
                            putNumber(fields,"credits",member(heroClass,"creditsAmount"));
                            putNumber(fields,"uridium",member(heroClass,"uridiumAmount"));
                            putNumber(fields,"experience",member(heroClass,"experiencePoints"));
                            putNumber(fields,"honor",member(heroClass,"honorPoints"));
                            putNumber(fields,"localTargetId",localTargetValue(resolvePath("net.bigpoint.darkorbit.settings.Settings")));
                        }
                        putNumber(fields,"speed",nestedValue(member(entity,"moveTrait"),"speed","value"));
                        fields.isValid = member(entity,"container") != null;
                        var lock:Object=member(entity,"attackTarget");
                        var lockType:*=nestedValue(lock,"lockType","value");
                        if (npc && (lockType===1 || lockType===2)) fields.isOwned=lockType===1;
                        var targetTrait:Object=member(member(entity,"attackTrait"),"target");
                        var targetOwner:Object=member(targetTrait,"owner");
                        if (targetOwner!=null) fields.attackingHero=callBoolean(targetOwner,"isHeroShip");
                        if (fields.isHero) fields.attackingTargetId=targetOwner==null ? null : member(targetOwner,"id");
                        putNumber(fields, "health", nestedValue(hpTrait, "hp", "value"));
                        putNumber(fields, "maxHealth", nestedValue(hpTrait, "maxHP", "value"));
                        putNumber(fields, "shield", nestedValue(hpTrait, "shield", "value"));
                        putNumber(fields, "maxShield", nestedValue(hpTrait, "maxShield", "value"));
                        putNumber(fields, "nanohullHP", nestedValue(hpTrait, "nanohullHP", "value"));
                        putNumber(fields, "nanohullMaxHP", nestedValue(hpTrait, "nanohullMaxHP", "value"));
                    }

                    var hash:* = member(entity, "hash");
                    if (hash is String)
                    {
                        fields.isCollectable = true;
                        fields.hash = String(hash);
                    }
                    var typeID:* = member(entity, "typeID");
                    if (typeID is Number)
                    {
                        fields.typeID = Number(typeID);
                    }
                    var classSeparator:int = className.lastIndexOf("::");
                    var classDot:int = className.lastIndexOf(".");
                    var simpleName:String = className.substr(
                        Math.max(classSeparator + 2, classDot + 1));
                    if (simpleName == "Box")
                    {
                        fields.entityKind = "box";
                    }
                    else if (simpleName == "Ore")
                    {
                        fields.entityKind = "ore";
                    }
                    else if (simpleName == "Beacon")
                    {
                        fields.entityKind = "beacon";
                    }

                    var name:String = className;
                    var info:Object = member(entity, "info");
                    var username:* = nestedValue(info, "username", "value");
                    if (username is String && String(username) != "")
                    {
                        name = String(username);
                    }
                    else if (hash is String && String(hash) != "")
                    {
                        name = String(hash);
                    }
                    if (compact) {
                        var packed:Object={};
                        for (var field:String in fields) if (field in _packedIndices) packed[_packedIndices[field]]=fields[field];
                        result.push([id,simpleName,name,stageX,stageY,worldX,worldY,packed]);
                    } else result.push({path: "map/" + String(id), className: simpleName,
                        name: name, x: stageX, y: stageY,
                        fields: fields});
                    if (result.length >= 2000)
                    {
                        break;
                    }
                }
                return result;
            }
            catch (error:Error)
            {
                return null;
            }
            return null;
        }

        /** Current world-space map data used by the controller's live map. */
        private static function mapSnapshot():Object
        {
            try
            {
                var screen:Object = resolvePath(
                    "net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
                var gameMap:Object = member(screen, "map");
                if (gameMap == null)
                {
                    return null;
                }

                var mapId:Number = Number(member(gameMap, "mapID"));
                if (isNaN(mapId))
                {
                    mapId = Number(callMember(gameMap, "getMapID"));
                }
                var mapName:* = callMember(gameMap, "getName");
                var mapWidth:Number = Number(member(gameMap, "serious_width"));
                var mapHeight:Number = Number(member(gameMap, "serious_height"));
                if (isNaN(mapWidth) || mapWidth <= 0)
                {
                    mapWidth = 21000;
                }
                if (isNaN(mapHeight) || mapHeight <= 0)
                {
                    mapHeight = 13100;
                }

                var hero:Object = member(gameMap, "hero");
                var heroPosition:Object = member(hero, "position");
                var playerX:Number = Number(member(heroPosition, "x"));
                var playerY:Number = Number(member(heroPosition, "y"));
                var hasPlayerPosition:Boolean = heroPosition != null &&
                    !isNaN(playerX) && !isNaN(playerY);
                var playerPosition:Object = hasPlayerPosition ?
                    {x: playerX, y: playerY} : null;

                var safePlaces:Array = [];
                var source:* = member(gameMap, "entities");
                if (source != null)
                {
                    for each (var entity:Object in source)
                    {
                        if (entity == null)
                        {
                            continue;
                        }
                        var className:String = getQualifiedClassName(entity);
                        var separator:int = className.lastIndexOf("::");
                        var dot:int = className.lastIndexOf(".");
                        var simpleName:String = className.substr(
                            Math.max(separator + 2, dot + 1));
                        var kind:String = "";
                        var radius:Number = 250;
                        if (simpleName.indexOf("Portal") >= 0)
                        {
                            kind = "portal";
                        }
                        else if (simpleName.indexOf("BattleStation") >= 0)
                        {
                            kind = "battle_station";
                        }
                        else if (simpleName.indexOf("BaseSpot") >= 0 ||
                            simpleName == "BaseStation" ||
                            simpleName == "BaseHeadquarters")
                        {
                            kind = "base";
                            if (simpleName.indexOf("Headquarters") >= 0)
                            {
                                radius = 1250;
                            }
                            else if (simpleName.indexOf("BaseStation") >= 0)
                            {
                                radius = 1500;
                            }
                        }
                        if (kind == "")
                        {
                            continue;
                        }

                        var safePosition:Object = member(entity, "position");
                        var safeX:Number = Number(member(safePosition, "x"));
                        var safeY:Number = Number(member(safePosition, "y"));
                        if (safePosition == null || isNaN(safeX) || isNaN(safeY))
                        {
                            continue;
                        }
                        var safeName:* = member(entity, "name");
                        safePlaces.push({kind: kind,
                            name: safeName is String ? String(safeName) : simpleName,
                            x: safeX, y: safeY, radius: radius});
                    }
                }

                var inSafePlace:Boolean = false;
                if (hasPlayerPosition)
                {
                    for each (var safePlace:Object in safePlaces)
                    {
                        var dx:Number = playerX - Number(safePlace.x);
                        var dy:Number = playerY - Number(safePlace.y);
                        var safeRadius:Number = Number(safePlace.radius);
                        if (dx * dx + dy * dy <= safeRadius * safeRadius)
                        {
                            inSafePlace = true;
                            break;
                        }
                    }
                }

                return {
                    id: isNaN(mapId) ? null : int(mapId),
                    name: mapName is String ? String(mapName) : "",
                    width: mapWidth,
                    height: mapHeight,
                    playerPosition: playerPosition,
                    safePlaces: safePlaces,
                    inSafePlace: hasPlayerPosition ? inSafePlace : null
                };
            }
            catch (error:Error)
            {
                return null;
            }
            return null;
        }

        private static function member(value:Object, name:String):*
        {
            if (value == null)
            {
                return null;
            }
            try
            {
                return name in value ? value[name] : null;
            }
            catch (error:Error)
            {
            }
            return null;
        }

        private static function callMember(target:Object, method:String):*
        {
            var callable:* = member(target, method);
            if (callable is Function)
            {
                try
                {
                    return (callable as Function).apply(target, []);
                }
                catch (error:Error)
                {
                }
            }
            return null;
        }

        private static function nestedValue(value:Object, first:String, second:String):*
        {
            return member(member(value, first), second);
        }

        // Settings.localTargetId is a plain Number on some builds and a
        // property wrapper (`.value`) on others; unwrap either.
        private static function localTargetValue(settings:Object):*
        {
            // `localTargetId` was removed from current builds; the selection
            // now lives under selectedTarget/lockedTarget (a Number, a
            // bindable `.value`, or the target ship itself).
            // The game's own selection lives on the map's ShipManager, set the
            // moment a ship is clicked (before any attack starts).
            try {
                var screen:Object = resolvePath("net.bigpoint.darkorbit.managers.ScreenManager.getInstance()");
                var shipManager:Object = callMethod(member(screen, "map"), "getShipManager");
                var selected:Object = callMethod(shipManager, "getSelectedShip");
                var selectedId:* = member(selected, "id");
                if (selectedId is Number && Number(selectedId) > 0) return selectedId;
            } catch (selectionError:Error) {}
            var names:Array = ["localTargetId", "selectedTarget", "lockedTarget", "targetId"];
            for each (var name:String in names) {
                var raw:* = member(settings, name);
                if (raw == null) continue;
                if (raw is Number) return raw;
                var inner:* = member(raw, "value");
                if (inner is Number) return inner;
                if (inner != null) raw = inner;
                var id:* = member(raw, "id");
                if (id is Number) return id;
            }
            return null;
        }

        private static function putNumber(target:Object, key:String, value:*):void
        {
            if (value is Number && !isNaN(Number(value)))
            {
                target[key] = Number(value);
            }
        }

        private static function callMethod(target:Object, method:String):Object
        {
            var callable:* = member(target, method);
            if (callable is Function)
            {
                try
                {
                    return (callable as Function).apply(target, []);
                }
                catch (error:Error)
                {
                }
            }
            return null;
        }

        private static function callBoolean(target:Object, method:String):Boolean
        {
            var callable:* = member(target, method);
            if (callable is Function)
            {
                try
                {
                    return Boolean((callable as Function).apply(target, []));
                }
                catch (error:Error)
                {
                }
            }
            return false;
        }

        private static function snapshot(object:DisplayObject, output:Array,
                                          stats:Object, path:String, depth:int):void
        {
            if (object == null || depth > 64 || stats.visited >= 5000)
            {
                stats.truncated = true;
                return;
            }
            stats.visited++;
            if (!object.visible || object.alpha <= 0)
            {
                return;
            }
            var position:Point;
            var bounds:Rectangle;
            try
            {
                position = object.localToGlobal(new Point(0, 0));
                bounds = object.getBounds(object.stage == null ? object : object.stage);
            }
            catch (error:Error)
            {
                return;
            }
            var width:Number = object.stage == null ? NaN : object.stage.stageWidth;
            var height:Number = object.stage == null ? NaN : object.stage.stageHeight;
            var onScreen:Boolean = object.stage == null ||
                (bounds.right >= 0 && bounds.bottom >= 0 &&
                 bounds.left <= width && bounds.top <= height);
            if (!onScreen)
            {
                return;
            }
            var fields:Object = {};
            var className:String = getQualifiedClassName(object);
            var names:Array = stateFields(className);
            for each (var field:String in names)
            {
                try
                {
                    var value:* = object[field];
                    if (value is String || value is Number || value is Boolean)
                    {
                        fields[field] = value;
                    }
                }
                catch (error:Error)
                {
                }
            }
            // Ignore tiny leaf graphics; retain containers and any object with
            // gameplay fields, including portals and ships with no children.
            var container:DisplayObjectContainer = object as DisplayObjectContainer;
            if (depth == 0 || (bounds.width > 1 && bounds.height > 1 &&
                (container != null || hasFields(fields))))
            {
                output.push({path: path, className: className,
                    name: object.name, x: position.x, y: position.y,
                    bounds: {x: bounds.x, y: bounds.y, width: bounds.width,
                             height: bounds.height}, fields: fields});
            }
            if (container != null)
            {
                for (var index:int = 0; index < container.numChildren; index++)
                {
                    if (stats.visited >= 5000)
                    {
                        stats.truncated = true;
                        return;
                    }
                    try
                    {
                        snapshot(container.getChildAt(index), output, stats,
                            path + "/" + index, depth + 1);
                    }
                    catch (error:Error)
                    {
                    }
                }
            }
        }

        private static function hasFields(fields:Object):Boolean
        {
            for (var key:String in fields)
            {
                return true;
            }
            return false;
        }

        private static function stateFields(className:String):Array
        {
            if (_stateFields[className] != null)
            {
                return _stateFields[className] as Array;
            }
            // Keep the visual fallback bounded instead of reflecting every
            // display object while walking the stage.
            var names:Array = ["health", "maxHealth", "currentHealth", "healthPoints",
                "hp", "maxHp", "hitPoints", "maxHitPoints", "shield", "maxShield",
                "id", "userId", "type", "typeID", "faction", "portalId", "isHero",
                "isNpc", "isNPC", "isHeroShip", "hash"];
            _stateFields[className] = names;
            return names;
        }

        private static function collectMembers(
            list:XMLList, owner:String, methods:Array, accessors:Array,
            variables:Array, asMethods:Boolean):void
        {
            for each (var node:XML in list)
            {
                var name:String = String(node.@name);
                if (asMethods)
                {
                    if (methods.length >= 400)
                    {
                        return;
                    }
                    methods.push({
                        name: name,
                        params: node.parameter.length(),
                        owner: owner
                    });
                }
                else if (String(node.name()) == "accessor")
                {
                    if (accessors.length < 400)
                    {
                        accessors.push({name: name, access: String(node.@access), owner: owner});
                    }
                }
                else if (variables.length < 400)
                {
                    variables.push({name: name, owner: owner});
                }
            }
        }
    }
}

/**
 * Discovery root for native capture. The slot layout is fixed by the AVM
 * (32-bit, then pointer, then Number slots): screen at +32, tag0 at +40 and
 * tag1 at +48 on x64. native_state.cpp relies on this; keep the fields as is.
 */
final class NativeAnchor
{
    public var tag0:Number = 0;
    public var tag1:Number = 0;
    public var screen:Object = null;
}
